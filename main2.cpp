/*
 * This file is derived from libopencm3 example code.
 *
 * Copyright (C) 2010 Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#define PRNT(x)
#define PRNTLN(x)
#include <mculib/fastwiring.hpp>
#include <mculib/softi2c.hpp>
#include <mculib/si5351.hpp>
#include <mculib/dma_adc.hpp>
#include <mculib/usbserial.hpp>

#include <array>
#include <complex>

#include "main.hpp"
#include <board.hpp>
#include "ili9341.hpp"
#include "plot.hpp"
#include "uihw.hpp"
#include "ui.hpp"
#include "common.hpp"
#include "globals.hpp"
#include "synthesizers.hpp"
#include "vna_measurement.hpp"
#include "printf.h"
#include "fifo.hpp"

#include <libopencm3/stm32/timer.h>

using namespace mculib;
using namespace std;
using namespace board;

// see https://lists.debian.org/debian-gcc/2003/07/msg00057.html
// this can be any value since we are not using shared libraries.
void* __dso_handle = (void*) &__dso_handle;

bool outputRawSamples = false;
int cpu_mhz = 24;


USBSerial serial;

static const int adcBufSize=4096;	// must be power of 2
volatile uint16_t adcBuffer[adcBufSize];

VNAMeasurement vnaMeasurement;


struct usbDataPoint {
	VNAObservation value;
	int freqIndex;
};
usbDataPoint usbTxQueue[64];
constexpr int usbTxQueueMask = 63;
volatile int usbTxQueueWPos = 0;
volatile int usbTxQueueRPos = 0;

// periods of a 1MHz clock; how often to call adc_process()
static constexpr int tim1Period = 25;	// 1MHz / 25 = 40kHz

// value is in microseconds; increments at 40kHz by TIM1 interrupt
volatile uint32_t systemTimeCounter = 0;

FIFO<small_function<void()>, 8> eventQueue;

volatile bool usbDataMode = false;

void adc_process();

template<unsigned int N>
static inline void pinMode(const array<Pad, N>& p, int mode) {
	for(int i=0; i<N; i++)
		pinMode(p[i], mode);
}

void errorBlink(int cnt) {
	digitalWrite(led, HIGH);
	while (1) {
		for(int i=0;i<cnt;i++) {
			digitalWrite(led, HIGH);
			delay(200);
			digitalWrite(led, LOW);
			delay(200);
		}
		delay(1000);
	}
}


void timer_setup() {
	// set the timer to count one tick per us
	rcc_periph_clock_enable(RCC_TIM1);
	rcc_periph_reset_pulse(RST_TIM1);
	timer_set_mode(TIM1, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);
	timer_set_prescaler(TIM1, cpu_mhz-1);
	timer_set_repetition_counter(TIM1, 0);
	timer_continuous_mode(TIM1);
	
	// this doesn't really set the period, but the "autoreload value"; actual period is this plus 1.
	// this should be fixed in libopencm3.
	
	timer_set_period(TIM1, tim1Period - 1);

	timer_enable_preload(TIM1);
	timer_enable_preload_complementry_enable_bits(TIM1);
	timer_enable_break_main_output(TIM1);
	
	timer_enable_irq(TIM1, TIM_DIER_UIE);
	nvic_enable_irq(NVIC_TIM1_UP_IRQ);
	
	TIM1_EGR = TIM_EGR_UG;
	timer_set_counter(TIM1, 0);
	timer_enable_counter(TIM1);
}
extern "C" void tim1_up_isr() {
	TIM1_SR = 0;
	systemTimeCounter += tim1Period;
	adc_process();
	UIHW::checkButtons();
}

void si5351_doUpdate(uint32_t freq_khz) {
	si5351_set(true, freq_khz+lo_freq/1000);
	si5351_set(false, freq_khz);
	si5351.PLLReset2();
}

void si5351_update(uint32_t freq_khz) {
	static uint32_t prevFreq = 0;
	si5351_doUpdate(freq_khz);
	if(freq_khz < prevFreq)
		si5351_doUpdate(freq_khz);
	prevFreq = freq_khz;
}



void adf4350_setup() {
	adf4350_rx.N = 120;
	adf4350_rx.rfPower = 0b11;
	adf4350_rx.sendConfig();
	adf4350_rx.sendN();

	adf4350_tx.N = 120;
	adf4350_tx.rfPower = 0b11;
	adf4350_tx.sendConfig();
	adf4350_tx.sendN();
}
void adf4350_update(uint32_t freq_khz) {
	freq_khz = uint32_t(freq_khz/adf4350_freqStep) * adf4350_freqStep;
	synthesizers::adf4350_set(adf4350_tx, freq_khz);
	synthesizers::adf4350_set(adf4350_rx, freq_khz + lo_freq/1000);
}


// set the measurement frequency including setting the tx and rx synthesizers
void setFrequency(uint32_t freq_khz) {
	if(freq_khz > 2700000)
		rfsw(RFSW_BBGAIN, RFSW_BBGAIN_GAIN(3));
	else if(freq_khz > 1500000)
		rfsw(RFSW_BBGAIN, RFSW_BBGAIN_GAIN(2));
	else
		rfsw(RFSW_BBGAIN, RFSW_BBGAIN_GAIN(1));

	// use adf4350 for f > 140MHz
	if(freq_khz > 140000) {
		adf4350_update(freq_khz);
		rfsw(RFSW_TXSYNTH, RFSW_TXSYNTH_HF);
		rfsw(RFSW_RXSYNTH, RFSW_RXSYNTH_HF);
	} else {
		si5351_update(freq_khz);
		rfsw(RFSW_TXSYNTH, RFSW_TXSYNTH_LF);
		rfsw(RFSW_RXSYNTH, RFSW_RXSYNTH_LF);
	}
}

void adc_setup() {
	static uint8_t channel_array[16] = {1};
	dmaADC.buffer = adcBuffer;
	dmaADC.bufferSizeBytes = sizeof(adcBuffer);
	dmaADC.init(channel_array, 1);

	adc_set_sample_time_on_all_channels(dmaADC.adcDevice, adc_ratecfg);
	dmaADC.start();
}

// read and consume data from the adc ring buffer
void adc_read(volatile uint16_t*& data, int& len) {
	static uint32_t lastIndex = 0;
	uint32_t cIndex = dmaADC.position();
	uint32_t bufWords = dmaADC.bufferSizeBytes / 2;
	cIndex &= (bufWords-1);
	
	data = ((volatile uint16_t*) dmaADC.buffer) + lastIndex;
	if(cIndex >= lastIndex) {
		len = cIndex - lastIndex;
	} else {
		len = bufWords - lastIndex;
	}
	lastIndex += len;
	if(lastIndex >= bufWords) lastIndex = 0;
}


void lcd_setup() {
	lcd_spi_init();

	ili9341_conf_cs = ili9341_cs;
	ili9341_conf_dc = ili9341_dc;
	ili9341_spi_transfer = [](uint32_t sdi, int bits) {
		return lcd_spi_transfer(sdi, bits);
	};
	ili9341_spi_transfer_bulk = [](uint32_t words) {
		int bytes = words*2;
		lcd_spi_transfer_bulk((uint8_t*)ili9341_spi_buffer, words*2);
	};
	ili9341_spi_wait_bulk = []() {
		lcd_spi_waitDMA();
	};

	xpt2046.spiTransfer = [](uint32_t sdi, int bits) {
		lcd_spi_waitDMA();
		digitalWrite(ili9341_cs, HIGH);
		
		lcd_spi_slow();
		delayMicroseconds(10);
		uint32_t ret = lcd_spi_transfer(sdi, bits);
		delayMicroseconds(10);
		lcd_spi_fast();
		return ret;
	};
	delay(10);

	xpt2046.begin(320, 240);
	
	ili9341_init();
	ili9341_test(5);
	plot_init();
	//redraw_frame();
	//redraw_request = REDRAW_CELLS | REDRAW_FREQUENCY | REDRAW_CAL_STATUS;
	//draw_all(true);
	ui_init();
	//handle_touch_interrupt();
	//ui_process();
	//ui_mode_menu();
	plot_into_index(measured);
	redraw_request |= REDRAW_CELLS;
	draw_all(true);
}



/*
-- register map:
-- 00: pll_frequency [23..16]
-- 01: pll_frequency [15..8]
-- 02: pll_frequency [7..0]
-- 03: update_trigger; write 1 to update all plls
-- 04: attenuation, in 0.5dB increments
-- 05: 
--   [1..0]: signal generator output power
--   [3..2]: LO output power
--   [7..4]: output data mode:
--				0: normal
--				1: adc0 data (filtered and decimated to 19.2MSPS)
--				2: adc1 data (filtered and decimated to 19.2MSPS)
--				3: adc0 data unfiltered (downsampled to 19.2MSPS)
--				4: adc1 data unfiltered (downsampled to 19.2MSPS)
-- note: the output signal frequency is pll_frequency * 10kHz*/

void serialCharHandler(uint8_t* s, int len) {
	static uint8_t writingRegister = 255;
	for(int i=0;i<len;i++) {
		uint8_t ch=s[i];
		if(writingRegister == 255) // set write address
			writingRegister = (ch == 0 ? 255 : (ch - 1));
		else { // write reg
			if(writingRegister < sizeof(registers)) {
				registers[writingRegister] = ch;
			}
			if(writingRegister == 3) {
				// update pll
				uint32_t freq = (uint32_t(registers[0]) << 16)
						| (uint32_t(registers[1]) << 8)
						| (uint32_t(registers[2]) << 0);
				
				//setFrequency(freq*10);
				vnaMeasurement.setSweep(freq*10000, 0, 1);
				usbDataMode = true;
			}
			if(writingRegister == 5) {
				uint8_t outpMode = ch >> 4;
				if(outpMode > 0)
					outputRawSamples = true;
				else
					outputRawSamples = false;
			}
			writingRegister = 255;
		}
	}
}





// callback called by VNAMeasurement to change rf switch positions.
void measurementPhaseChanged(VNAMeasurementPhases ph) {
	switch(ph) {
		case VNAMeasurementPhases::REFERENCE:
			rfsw(RFSW_REFL, RFSW_REFL_ON);
			rfsw(RFSW_RECV, RFSW_RECV_REFL);
			rfsw(RFSW_ECAL, RFSW_ECAL_SHORT);
			break;
		case VNAMeasurementPhases::REFL1:
			rfsw(RFSW_ECAL, RFSW_ECAL_NORMAL);
			break;
		case VNAMeasurementPhases::REFL2:
			rfsw(RFSW_ECAL, RFSW_ECAL_NORMAL);
			break;
		case VNAMeasurementPhases::THRU:
			rfsw(RFSW_ECAL, RFSW_ECAL_NORMAL);
			rfsw(RFSW_REFL, RFSW_REFL_OFF);
			rfsw(RFSW_RECV, RFSW_RECV_PORT2);
			break;
	}
}

// callback called by VNAMeasurement when an observation is available.
static void measurementEmitDataPoint(int freqIndex, uint64_t freqHz, const VNAObservation& v) {
	// enqueue new data point
	int wrRPos = usbTxQueueRPos;
	int wrWPos = usbTxQueueWPos;
	__sync_synchronize();
	if((wrWPos + 1) & usbTxQueueMask == wrRPos) {
		// overflow
	} else {
		usbTxQueue[wrWPos].freqIndex = freqIndex;
		usbTxQueue[wrWPos].value = v;
		__sync_synchronize();
		usbTxQueueWPos = (wrWPos + 1) & usbTxQueueMask;
	}
}


void updateSweepParams() {
	uint64_t start = current_props._frequency0;
	uint64_t step = (current_props._frequency1 - current_props._frequency0) / current_props._sweep_points;
	vnaMeasurement.setSweep(start, step, current_props._sweep_points, 1);
}

void measurement_setup() {
	vnaMeasurement.phaseChanged = [](VNAMeasurementPhases ph) {
		measurementPhaseChanged(ph);
	};
	vnaMeasurement.emitDataPoint = [](int freqIndex, uint64_t freqHz, const VNAObservation& v) {
		measurementEmitDataPoint(freqIndex, freqHz, v);
	};
	vnaMeasurement.frequencyChanged = [](uint64_t freqHz) {
		setFrequency(freqHz/1000);
	};
	vnaMeasurement.init();
	updateSweepParams();
}

void adc_process() {
	if(!outputRawSamples) {
		volatile uint16_t* buf;
		int len;
		for(int i=0; i<2; i++) {
			adc_read(buf, len);
			vnaMeasurement.sampleProcessor.clipFlag = false;
			vnaMeasurement.processSamples((uint16_t*)buf, len);
			digitalWrite(led, vnaMeasurement.sampleProcessor.clipFlag?1:0);
		}
	}
}

// transmit any outstanding data in the usbTxQueue
void usb_transmit() {
	int rdRPos = usbTxQueueRPos;
	int rdWPos = usbTxQueueWPos;
	__sync_synchronize();

	if(rdRPos == rdWPos) // queue empty
		return;
	
	usbDataPoint& usbDP = usbTxQueue[rdRPos];
	VNAObservation& value = usbDP.value;
	int32_t fwdRe = value[1].real();
	int32_t fwdIm = value[1].imag();
	int32_t reflRe = value[0].real();
	int32_t reflIm = value[0].imag();
	int32_t thruRe = value[2].real();
	int32_t thruIm = value[2].imag();
	uint8_t txbuf[31];
	txbuf[0] = fwdRe & 0x7F;
	txbuf[1] = (fwdRe >> 7) | 0x80;
	txbuf[2] = (fwdRe >> 14) | 0x80;
	txbuf[3] = (fwdRe >> 21) | 0x80;
	txbuf[4] = (fwdRe >> 28) | 0x80;
	
	txbuf[5] = (fwdIm >> 0) | 0x80;
	txbuf[6] = (fwdIm >> 7) | 0x80;
	txbuf[7] = (fwdIm >> 14) | 0x80;
	txbuf[8] = (fwdIm >> 21) | 0x80;
	txbuf[9] = (fwdIm >> 28) | 0x80;
	
	txbuf[10] = (reflRe >> 0) | 0x80;
	txbuf[11] = (reflRe >> 7) | 0x80;
	txbuf[12] = (reflRe >> 14) | 0x80;
	txbuf[13] = (reflRe >> 21) | 0x80;
	txbuf[14] = (reflRe >> 28) | 0x80;
	
	txbuf[15] = (reflIm >> 0) | 0x80;
	txbuf[16] = (reflIm >> 7) | 0x80;
	txbuf[17] = (reflIm >> 14) | 0x80;
	txbuf[18] = (reflIm >> 21) | 0x80;
	txbuf[19] = (reflIm >> 28) | 0x80;
	
	txbuf[20] = (thruRe >> 0) | 0x80;
	txbuf[21] = (thruRe >> 7) | 0x80;
	txbuf[22] = (thruRe >> 14) | 0x80;
	txbuf[23] = (thruRe >> 21) | 0x80;
	txbuf[24] = (thruRe >> 28) | 0x80;
	
	txbuf[25] = (thruIm >> 0) | 0x80;
	txbuf[26] = (thruIm >> 7) | 0x80;
	txbuf[27] = (thruIm >> 14) | 0x80;
	txbuf[28] = (thruIm >> 21) | 0x80;
	txbuf[29] = (thruIm >> 28) | 0x80;
	
	uint8_t checksum=0b01000110;
	for(int i=0; i<30; i++)
		checksum = (checksum xor ((checksum<<1) | 1)) xor txbuf[i];
	txbuf[30] = checksum | (1<<7);
	
	serial.print((char*)txbuf, sizeof(txbuf));
	__sync_synchronize();
	usbTxQueueRPos = (rdRPos + 1) & usbTxQueueMask;
}

void usb_transmit_rawSamples() {
	volatile uint16_t* buf;
	int len;
	adc_read(buf, len);
	int8_t tmpBuf[adcBufSize];
	for(int i=0; i<len; i++)
		tmpBuf[i] = int8_t(buf[i] >> 4) - 128;
	serial.print((char*)tmpBuf, len);
	rfsw(RFSW_ECAL, RFSW_ECAL_NORMAL);
	//rfsw(RFSW_RECV, ((cnt / 500) % 2) ? RFSW_RECV_REFL : RFSW_RECV_PORT2);
	//rfsw(RFSW_REFL, ((cnt / 500) % 2) ? RFSW_REFL_ON : RFSW_REFL_OFF);
	rfsw(RFSW_RECV, RFSW_RECV_REFL);
	rfsw(RFSW_REFL, RFSW_REFL_ON);
}

void processDataPoint() {
	int rdRPos = usbTxQueueRPos;
	int rdWPos = usbTxQueueWPos;
	__sync_synchronize();

	while(rdRPos != rdWPos) {
		usbDataPoint& usbDP = usbTxQueue[rdRPos];
		VNAObservation& value = usbDP.value;
		measured[0][usbDP.freqIndex] = value[0]/value[1];
		
		rdRPos = (rdRPos + 1) & usbTxQueueMask;
	}
	usbTxQueueRPos = rdRPos;
}

int main(void) {
	int i;
	boardInit();

	pinMode(led, OUTPUT);
	pinMode(led2, OUTPUT);
	pinMode(RFSW_ECAL, OUTPUT);
	pinMode(RFSW_BBGAIN, OUTPUT);
	pinMode(RFSW_TXSYNTH, OUTPUT);
	pinMode(RFSW_RXSYNTH, OUTPUT);
	pinMode(RFSW_REFL, OUTPUT);
	pinMode(RFSW_RECV, OUTPUT);

	digitalWrite(led, HIGH);
	
	rfsw(RFSW_BBGAIN, RFSW_BBGAIN_GAIN(0));
	rfsw(RFSW_RXSYNTH, RFSW_RXSYNTH_LF);
	rfsw(RFSW_TXSYNTH, RFSW_TXSYNTH_LF);
	rfsw(RFSW_REFL, RFSW_REFL_ON);
	rfsw(RFSW_RECV, RFSW_RECV_REFL);
	rfsw(RFSW_ECAL, RFSW_ECAL_NORMAL);

	delay(200);

	serial.setReceiveCallback([](uint8_t* s, int len) {
		serialCharHandler(s, len);
	});
	// baud rate is ignored for usbserial
	serial.begin(115200);

	lcd_setup();
	UIHW::init(tim1Period);

	si5351_i2c.init();
	if(!si5351_setup())
		errorBlink(2);

	setFrequency(56000);

	measurement_setup();
	adc_setup();
	timer_setup();

	adf4350_setup();


	bool testSG = false;
	
	if(testSG) {
		while(1) {
			uint16_t tmp = 1;
			vnaMeasurement.processSamples(&tmp, 1);
		}
		return 0;
	}

	timer_setup();
	
	bool lastUSBDataMode = false;
	while(true) {
		if(usbDataMode) {
			usb_transmit();
			// display "usb mode" screen
			if(!lastUSBDataMode)
				show_usb_data_mode();
			lastUSBDataMode = usbDataMode;
			continue;
		}
		lastUSBDataMode = usbDataMode;
		processDataPoint();
		
		plot_into_index(measured);
		
		if(!eventQueue.readable()) {
			draw_all(true);
			continue;
		}
		auto callback = eventQueue.read();
		eventQueue.dequeue();
		if(!callback)
			abort();
		callback();
	}
}

extern "C" void abort() {
	while (1) {
		for(int i=0;i<3;i++) {
			digitalWrite(led, HIGH);
			delay(100);
			digitalWrite(led, LOW);
			delay(100);
		}
		delay(1000);
	}
}
/*
extern "C" void *memcpy(char *dest, const char *src, uint32_t n) {
	for(int i=0;i<n;i++) dest[i] = src[i];
	return dest;
}*/



// nanovna UI callbacks
namespace UIActions {

	void cal_collect(int type) {
		
	}
	void cal_done(void) {
		
	}


	void set_sweep_frequency(SweepParameter type, int32_t frequency) {
		switch(type) {
			case ST_START:
				current_props._frequency0 = frequency;
				break;
			case ST_STOP:
				current_props._frequency1 = frequency;
				break;
			default: return;
		}
		updateSweepParams();
	}
	uint32_t get_sweep_frequency(int type) {
		
	}

	void toggle_sweep(void) {
		
	}



	void set_trace_type(int t, int type) {
		
	}
	void set_trace_channel(int t, int channel) {
		
	}
	void set_trace_scale(int t, float scale) {
		
	}
	void set_trace_refpos(int t, float refpos) {
		
	}

	void set_electrical_delay(float picoseconds) {
		
	}
	float get_electrical_delay(void) {
		
	}

	void apply_edelay_at(int i) {
		
	}

	void set_frequencies(uint32_t start, uint32_t stop, int16_t points) {
		
	}
	void update_frequencies(void) {
		
	}

	void application_doSingleEvent() {
		if(eventQueue.readable()) {
			auto callback = eventQueue.read();
			eventQueue.dequeue();
			if(!callback)
				abort();
			callback();
		}
	}
	void enqueueEvent(const small_function<void()>& cb) {
		eventQueue.enqueue(cb);
	}
}
