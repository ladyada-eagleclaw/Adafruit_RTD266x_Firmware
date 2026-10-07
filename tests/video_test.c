// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtd/io.h"
#include "rtd/panel.h"
#include "rtd/video.h"
void ddcci_service(void) {}

/* Host model covers register encoding and mode rejection. It cannot model
 * analog PLL lock, the filter's physical tap mapping, or panel image quality.
 */
const panel_t panel = {800, 480, 1000, 525, 88, 32, 48, 3, 31500000UL};
static uint8_t registers[7][256];
static uint8_t ports[7][256][256];
static uint8_t measurement[2][8];
static uint8_t coefficients[128];
static unsigned coefficient_count;
#if RTD_ASPECT_4_3 || RTD_ASPECT_16_9
static uint8_t downscale_coefficients[64];
static unsigned downscale_coefficient_count;
#endif
static uint32_t now;
static uint8_t stalled;
static unsigned indirect_writes;
static uint8_t checking_avmute;
static uint8_t background[3];
static unsigned background_bytes;
static uint8_t picture_coefficients[16];
static uint8_t color_coefficients[3][6];
static uint8_t color_offsets[3];
static uint8_t color_index;
static uint8_t color_rows_written;
static uint8_t color_burst;
static unsigned other_page_writes;

uint8_t rtd_read(uint8_t page, uint8_t reg) {
  return registers[page][reg];
}

void rtd_write(uint8_t page, uint8_t reg, uint8_t value) {
  if (page != 0) ++other_page_writes;
  if (page == 0 && reg == 0x62) {
    if ((value & 0x80) && !(registers[0][reg] & 0x80)) {
      assert((value & 0x3c) == 0x1c); /* Ready retains enabled blue-row access. */
      assert(color_rows_written == 7); /* All three complete rows precede latch. */
    }
    if (!(value & 0xb8)) color_rows_written = 0;
    color_index = 0; /* CR62 writes reset the selected row's byte pointer. */
  }
  if (page == 0 && reg == 0x35 && (value & 0x88) &&
      !(registers[0][reg] & 0x88)) coefficient_count = 0;
  registers[page][reg] = value;
#if RTD_ASPECT_4_3 || RTD_ASPECT_16_9
  if (page == 6 && reg == 0xf4) {
    assert(!(registers[6][0xe3] & 3));
    assert(downscale_coefficient_count < sizeof downscale_coefficients);
    downscale_coefficients[downscale_coefficient_count++] = value;
  }
#endif
  if (page == 0 && reg == 0x63) {
    uint8_t channel = (registers[0][0x62] >> 3) & 7;
    assert(channel && channel <= 6);
    if (channel <= 3) {
      assert(color_burst); /* Each complete row is one gateway transaction. */
      assert(color_index < 6);
      color_coefficients[channel - 1][color_index++] = value;
      if (color_index == 6) color_rows_written |= 1u << (channel - 1);
    } else {
      color_offsets[channel - 4] = value;
    }
  }
  if (page == 0 && reg == 0x65) {
    uint8_t index = registers[0][0x64] & 15;
    assert(registers[0][0x64] & 0x80);
    picture_coefficients[index] = value;
    registers[0][0x64] = (registers[0][0x64] & 0xf0) | ((index + 1) & 15);
  }
  if (page == 0 && reg == 0x6c && (value & 0x20)) background_bytes = 0;
  if (page == 0 && reg == 0x6d) {
    assert((registers[0][0x6c] & 0x20) && background_bytes < 3);
    background[background_bytes++] = value;
  }
  if (page == 0 && reg == 0x52 && (value & 0x60) && !(value & stalled)) {
    memcpy(&registers[0][0x52], measurement[registers[0][0x47] & 1], 8);
  }
  if (page == 0 && reg == 0x36) {
    uint8_t control = registers[0][0x35];
    assert((control & 0x88) == 0x88);
    assert((control & 0x44) != ((control & 0x22) << 1));
    assert(coefficient_count < sizeof(coefficients));
    coefficients[coefficient_count++] = value;
  }
  if (page == 0 && reg == 0x34) {
    assert(registers[0][0x33] & 0x80);
    ports[0][0x33][registers[0][0x33]++] = value;
  }
}

void rtd_write_bytes(uint8_t page, uint8_t reg, const uint8_t *values,
                     uint8_t count) {
  assert(page == 0 && reg == 0x63 && count == 6 && color_index == 0);
  color_burst = 1;
  while (count--) rtd_write(page, reg, *values++);
  color_burst = 0;
  assert(color_index == 6);
}

void rtd_update(uint8_t page, uint8_t reg, uint8_t mask, uint8_t value) {
  rtd_write(page, reg, (rtd_read(page, reg) & (uint8_t)~mask) | (value & mask));
}

uint8_t rtd_indirect_read(uint8_t page, uint8_t reg, uint8_t index) {
  return ports[page][reg][index];
}

void rtd_indirect_write(uint8_t page, uint8_t reg, uint8_t index, uint8_t value) {
  if (page != 0) ++other_page_writes;
  if (checking_avmute && page == 2 && reg == 0xc9 && index == 0x30)
    assert(!(ports[2][0xc9][0x31] & 0x80));
  ++indirect_writes;
  ports[page][reg][index] = value;
}

void rtd_indirect_update(uint8_t page, uint8_t reg, uint8_t index,
                         uint8_t mask, uint8_t value) {
  rtd_indirect_write(page, reg, index,
      (rtd_indirect_read(page, reg, index) & (uint8_t)~mask) | (value & mask));
}

uint32_t platform_millis(void) { return now++; }
void platform_delay_ms(uint16_t ms) { now += ms; }

static uint16_t word(uint8_t reg) {
  return ((uint16_t)(registers[0][reg] & 7) << 8) | registers[0][reg + 1];
}

static uint16_t timing(uint8_t index) {
  return ((uint16_t)ports[0][0x2a][index] << 8) | ports[0][0x2a][index + 1];
}

static uint32_t factor(uint8_t axis) {
  uint8_t index = 0x80 + axis * 3;
  return ((uint32_t)ports[0][0x33][index] << 16) |
         ((uint32_t)ports[0][0x33][index + 1] << 8) |
         ports[0][0x33][index + 2];
}

static void fixture(uint16_t width, uint16_t period) {
  uint16_t count = width == 640 ? 799 : 999;
  memset(measurement, 0, sizeof(measurement));
  measurement[1][0] = (uint8_t)(count >> 8);
  measurement[1][1] = (uint8_t)count;
  measurement[1][2] = 1;
  measurement[1][3] = 223; /* 479 active lines. */
  count = width - 1;
  measurement[1][4] = (uint8_t)((count >> 8) << 4);
  measurement[1][5] = (uint8_t)count;
  measurement[0][0] = (uint8_t)(period >> 12);
  measurement[0][1] = (uint8_t)(period >> 4);
  measurement[0][2] = 2;
  measurement[0][3] = 12; /* 524 total lines, accepted counter endpoint. */
  measurement[0][4] = period & 15;
  stalled = 0;
}

static void cvt_fixture(uint16_t period, uint16_t vtotal) {
  fixture(800, period);
  measurement[1][0] = 3;
  measurement[1][1] = 223; /* 991: 992-clock input total. */
  measurement[0][2] = 0x80 | (uint8_t)(vtotal >> 8);
  measurement[0][3] = (uint8_t)vtotal;
}

static uint32_t pll_estimate(void) {
  uint32_t base = 421875UL * (registers[1][0xbf] + 2);
  uint16_t offset = ((uint16_t)registers[1][0xc4] << 8) | registers[1][0xc5];
  assert(offset <= 4095);
  return base + (uint32_t)((uint64_t)base * offset / 32768);
}

static void assert_vertical(uint16_t total, uint16_t start) {
  assert(timing(0x0b) == total);
  assert(timing(0x0e) == start && timing(0x10) == start);
  assert(timing(0x12) == start + 480 && timing(0x14) == start + 480);
  assert((((uint16_t)(registers[1][0xc7] & 15) << 8) |
          registers[1][0xc8]) == total);
  assert(video_display_vstart() == start);
}

static void assert_picture(uint16_t width) {
  uint16_t margin = (800 - width) / 2;
  assert(timing(0) + 4 == 1000); /* Panel timing and DE stay full width. */
  assert(timing(3) + 10 == 88 && timing(9) - timing(3) == 800);
  assert(timing(5) - timing(3) == margin);
  assert(timing(9) - timing(7) == margin);
  assert(timing(7) - timing(5) == width);
  assert(factor(0) == 0xfffff && factor(1) == 0xfffff);
  assert((registers[0][0x32] & 0x13) == 0x10);
  assert(background_bytes == 3);
  assert(!background[0] && !background[1] && !background[2]);
}

static void cvt_profile(void) {
  video_signal_t signal;
  video_signal_t forged;
  uint16_t period;
  uint16_t total;
  uint32_t estimate;

  /* Both measured endpoints must configure the physical 500-line frame. */
  for (total = 499; total <= 500; ++total) {
    fixture(800, 13714);
    assert(video_measure(&signal) && signal.mode == VIDEO_MODE_PANEL);
    assert(video_apply(&signal));
    assert_vertical(525, 32);
    assert_picture(800);
    cvt_fixture(14501, total);
    assert(video_measure(&signal) && signal.mode == VIDEO_MODE_CVT);
    assert(signal.width == 800 && signal.height == 480);
    assert(signal.htotal == 992 && signal.vtotal == total && signal.polarity == 2);
    assert(signal.output_clock_hz == (uint32_t)(432000000000ULL / 14501));
    assert(video_apply(&signal));
    assert(word(0x14) == 166 && word(0x18) == 17);
    assert((registers[0][0x11] & 0x0c) == 0x04); /* Manual p22: bit2 H, bit3 V. */
    assert(registers[0][0x40] == 9 && registers[0][0x41] == 40);
    assert(factor(0) == 0xfffff && factor(1) == 0xfffff);
    assert_vertical(500, 10);
    assert_picture(800);
    assert((registers[0][0x28] & 0xa8) == 0x88);

    /* Invalid profile identifiers and profile/geometry mismatches do not
     * alter the currently displayed source. */
    forged = signal;
    forged.mode = VIDEO_MODE_NONE;
    assert(!video_apply(&forged));
    forged.mode = 255;
    assert(!video_apply(&forged));
    forged.mode = VIDEO_MODE_VGA;
    assert(!video_apply(&forged));
    forged.mode = VIDEO_MODE_PANEL;
    assert(!video_apply(&forged)); /* CVT clock outside panel profile. */
    assert_vertical(500, 10);
    assert(word(0x14) == 166);
  }

  video_blank(1);
  assert_vertical(525, 32);
  assert((registers[0][0x28] & 0xa8) == 0xa0);
  estimate = pll_estimate();
  assert(estimate > 31499000UL && estimate < 31501000UL);
  video_blank(1); /* Repeated no-signal updates preserve the restored raster. */
  assert_vertical(525, 32);

  cvt_fixture(14501, 500);
  assert(video_measure(&signal) && video_apply(&signal));
  fixture(640, 13714);
  assert(video_measure(&signal) && signal.mode == VIDEO_MODE_VGA);
  assert(video_apply(&signal));
  assert_vertical(525, 32);
  assert(word(0x14) == 142 && word(0x18) == 35);
  assert((registers[0][0x11] & 0x0c) == 0x0c);
  assert_picture(640);

  /* Independent 64-bit reference includes both accepted clock endpoints
   * and adjacent rejected periods, including fractional-Hz boundaries. */
  for (period = 14399; period <= 14645; ++period) {
    uint32_t expected = (uint32_t)(432000000000ULL / period);
    uint8_t accepted;
    cvt_fixture(period, 500);
    accepted = video_measure(&signal);
    assert(accepted == (expected >= 29500000UL && expected <= 30000000UL));
    if (accepted) {
      assert(signal.mode == VIDEO_MODE_CVT && signal.output_clock_hz == expected);
      assert(video_apply(&signal));
      estimate = pll_estimate();
      assert((estimate > expected ? estimate - expected : expected - estimate)
             < 1000);
    } else {
      assert(signal.error == VIDEO_LINE_RATE && signal.mode == VIDEO_MODE_NONE);
      assert(!signal.width && !signal.height && !signal.output_clock_hz);
    }
  }
  for (total = 498; total <= 501; total += 3) {
    cvt_fixture(14501, total);
    assert(!video_measure(&signal) && signal.error == VIDEO_VERTICAL_TOTAL);
  }
  cvt_fixture(14501, 500);
  ++measurement[1][1]; /* Similar width/rate is insufficient: total must match. */
  assert(!video_measure(&signal) && signal.error == VIDEO_DIGITAL_TOTAL);
  assert(signal.detail[0] == 993 && signal.detail[1] == 800);
}

static void polarity_profiles(void) {
  video_signal_t signal;
  uint8_t mode, polarity, saved_control;
  for (mode = VIDEO_MODE_VGA; mode <= VIDEO_MODE_CVT; ++mode) {
    for (polarity = 0; polarity < 4; ++polarity) {
      if (mode == VIDEO_MODE_CVT)
        cvt_fixture(14501, 500);
      else
        fixture(mode == VIDEO_MODE_VGA ? 640 : 800, 13714);
      measurement[0][2] = (measurement[0][2] & 0x3f) | (polarity << 6);
      assert(video_measure(&signal) && signal.mode == mode);
      assert(signal.polarity == polarity);
      registers[0][0x11] = 0xa3;
      assert(video_apply(&signal));
      assert(registers[0][0x11] == (uint8_t)(0xa3 | ((polarity ^ 3) << 2)));
      /* Re-measure after normalization: the host model supplies raw input
       * polarity independently of CR11; hardware must confirm that routing. */
      assert(video_measure(&signal) && signal.polarity == polarity);
      saved_control = registers[0][0x28];
      signal.polarity = 4;
      assert(!video_apply(&signal));
      signal.polarity = 255;
      assert(!video_apply(&signal));
      assert(registers[0][0x28] == saved_control);
      assert(registers[0][0x11] == (uint8_t)(0xa3 | ((polarity ^ 3) << 2)));
    }
  }
}

static void reject_cases(void) {
  video_signal_t signal;
  static const uint8_t errors[] = {
    VIDEO_GEOMETRY, VIDEO_GEOMETRY, VIDEO_DIGITAL_TOTAL,
    VIDEO_VERTICAL_TOTAL,
    VIDEO_ANALOG_OVERFLOW, VIDEO_ANALOG_TIMEOUT, VIDEO_ZERO_PERIOD,
    VIDEO_DIGITAL_OVERFLOW
  };
  unsigned i;
  for (i = 0; i < sizeof(errors); ++i) {
    fixture(800, 13714);
    switch (i) {
      case 0: fixture(720, 13714); break;
      case 1: measurement[1][3] = 224; break;
      case 2: measurement[1][1] = 0; break;
      case 3: measurement[0][3] = 11; break;
      case 4: measurement[0][0] |= 0x10; break;
      case 5: measurement[0][2] |= 0x20; break;
      case 6: fixture(800, 0); break;
      case 7: measurement[1][0] |= 0x10; break;
    }
    assert(!video_measure(&signal));
    assert(!signal.width && !signal.height && !signal.output_clock_hz);
    assert(signal.error == errors[i]);
    if (i == 0) {
      assert(signal.detail[0] == 1000 && signal.detail[1] == 720 &&
             signal.detail[2] == 480);
    }
    if (i == 4 || i == 5) {
      assert(signal.measured == VIDEO_MEASURE_GEOMETRY);
      assert(!signal.line_hz && !signal.vtotal && !signal.polarity);
    } else if (i == 7) {
      assert(!signal.measured && !signal.input_width && !signal.input_height);
      assert(!signal.htotal && !signal.vtotal && !signal.line_hz);
    }
  }
  fixture(800, 16000); /* 27 kHz: outside the admitted line-rate range. */
  assert(!video_measure(&signal));
  assert(signal.error == VIDEO_LINE_RATE && signal.detail[0] == 16000);
  fixture(800, 13714);
  stalled = 0x20;
  now = UINT32_MAX - 20;
  assert(!video_measure(&signal)); /* timeout remains bounded across wrap. */
  assert(signal.error == VIDEO_DIGITAL_TIMEOUT);
  assert(!signal.measured && !signal.input_width && !signal.input_height);
  assert(!signal.htotal && !signal.vtotal && !signal.line_hz && !signal.polarity);
  assert(!(registers[0][0x52] & 0x20));
  fixture(800, 13714);
  stalled = 0x40;
  assert(!video_measure(&signal)); /* Completed measurement, stalled pop-up. */
  assert(signal.error == VIDEO_DIGITAL_TIMEOUT);
  assert(!(registers[0][0x52] & 0x40));
  assert(!video_measure(NULL));
  assert(!video_apply(NULL));
}

static void unsupported_metadata(void) {
  video_signal_t signal;
  fixture(1024, 16500);
  measurement[1][0] = 5;
  measurement[1][1] = 63; /* 1343, digital total counts one short. */
  measurement[1][2] = 2;
  measurement[1][3] = 255; /* 767, digital active height counts one short. */
  measurement[0][2] = 0xc3;
  measurement[0][3] = 38; /* 806 physical lines, both syncs positive. */
  assert(!video_measure(&signal));
  assert(signal.error == VIDEO_GEOMETRY);
  assert(!signal.width && !signal.height && !signal.output_clock_hz);
  assert(signal.measured == (VIDEO_MEASURE_GEOMETRY | VIDEO_MEASURE_TIMING));
  assert(signal.input_width == 1024 && signal.input_height == 768);
  assert(signal.htotal == 1344 && signal.vtotal == 806);
  assert(signal.line_hz == 432000000UL / 16500 && signal.polarity == 3);
  assert(signal.detail[0] == 1344 && signal.detail[1] == 1024 &&
         signal.detail[2] == 768); /* Earlier digital error retains its detail. */

  measurement[0][2] |= 0x20; /* Analog timeout cannot supersede geometry. */
  assert(!video_measure(&signal));
  assert(signal.error == VIDEO_GEOMETRY);
  assert(signal.measured == VIDEO_MEASURE_GEOMETRY);
  assert(signal.input_width == 1024 && signal.input_height == 768);
  assert(!signal.vtotal && !signal.line_hz && !signal.polarity);
  assert(signal.detail[0] == 1344 && signal.detail[1] == 1024 &&
         signal.detail[2] == 768);
}

static void clock_range(void) {
  video_signal_t signal;
  uint16_t period;
  for (period = 13620; period < 13810; ++period) {
    uint32_t expected = (uint32_t)(432000000000ULL / period);
    uint8_t admitted;
    fixture(800, period);
    admitted = video_measure(&signal);
    assert(admitted == (expected >= 31300000UL && expected <= 31700000UL));
    if (admitted) {
      uint32_t base;
      uint32_t estimate;
      uint16_t offset;
      assert(signal.output_clock_hz == expected);
      assert(video_apply(&signal));
      assert(registers[1][0xc0] == 0x26);
      assert(registers[1][0xc1] == 0x81);
      base = 421875UL * (registers[1][0xbf] + 2);
      offset = ((uint16_t)registers[1][0xc4] << 8) | registers[1][0xc5];
      assert(offset <= 4095);
      estimate = base + (uint32_t)((uint64_t)base * offset / 32768);
      assert((estimate > expected ? estimate - expected : expected - estimate)
             < 1000); /* Fine-tune quantization and denominator rounding. */
    }
  }
}

static void avmute_recovery(void) {
  unsigned before;
  checking_avmute = 1;
  ports[2][0xc9][0x30] = 0xef; /* Audio enabled; unrelated bits populated. */
  ports[2][0xc9][0x31] = 0xb6;
  registers[2][0xcb] = 0x41; /* HDMI Set_AVMute. */
  before = indirect_writes;
  video_service();
  assert(ports[2][0xc9][0x30] == 0xe7);
  assert(ports[2][0xc9][0x31] == 0xb6);
  assert(indirect_writes == before + 3);
  before = indirect_writes;
  video_service();
  assert(indirect_writes == before);

  registers[2][0xcb] = 1; /* HDMI Clear_AVMute. */
  video_service();
  assert(ports[2][0xc9][0x30] == 0xef);
  assert(ports[2][0xc9][0x31] == 0xb6);
  before = indirect_writes;
  video_service();
  assert(indirect_writes == before);

  registers[2][0xcb] = 0x40; /* DVI ignores stale HDMI AVMute status. */
  ports[2][0xc9][0x30] = 0xe7;
  ports[2][0xc9][0x31] = 0x36; /* Preserve an already disabled watchdog. */
  before = indirect_writes;
  video_service();
  assert(ports[2][0xc9][0x30] == 0xef);
  assert(ports[2][0xc9][0x31] == 0x36);
  assert(indirect_writes == before + 1);
  checking_avmute = 0;
}

static void picture_controls(void) {
  static const uint8_t percent[] = {0, 25, 50, 75, 100, 255};
  static const uint8_t coefficient[] = {0, 64, 128, 191, 255, 255};
  unsigned i, channel;
  unsigned before = other_page_writes;
  registers[0][0x62] = 0x54;
  registers[0][0x64] = 0x4b;
  memset(picture_coefficients + 6, 0x37, 10);
  for (i = 0; i < sizeof percent; ++i) {
    video_set_picture(percent[i], percent[5 - i]);
    for (channel = 0; channel < 3; ++channel) {
      assert(picture_coefficients[channel] == coefficient[i]);
      assert(picture_coefficients[channel + 3] == coefficient[5 - i]);
    }
    for (channel = 6; channel < 16; ++channel)
      assert(picture_coefficients[channel] == 0x37);
    assert(registers[0][0x62] == 0x57);
    assert(registers[0][0x64] == 0x4b);
  }
  assert(other_page_writes == before); /* No receiver/audio/PLL changes. */
  video_set_picture(50, 50);
}

static void aspect_controls(void) {
  video_signal_t signal;
  unsigned before;
  uint8_t display;
  fixture(640, 13714);
  assert(video_measure(&signal) && video_apply(&signal));
  assert_picture(640);
  display = registers[0][0x28];
  before = other_page_writes;
  video_set_aspect(1);
  assert(timing(5) == timing(3) && timing(7) == timing(9));
  assert(timing(7) - timing(5) == 800);
  assert(factor(0) == 0xccccd && factor(1) == 0xfffff);
  assert((registers[0][0x32] & 0x13) == 0x11);
  assert(registers[0][0x28] == display && other_page_writes == before);
  video_set_aspect(0);
  assert_picture(640);
  assert(registers[0][0x28] == display && other_page_writes == before);

  video_set_aspect(VIDEO_ASPECT_FILL);
  video_set_aspect(255); /* Invalid values leave the previous choice intact. */
  fixture(800, 13714);
  assert(video_measure(&signal) && video_apply(&signal));
  assert_picture(800); /* Native input stays unity even when fill is selected. */
  fixture(640, 13714);
  assert(video_measure(&signal) && video_apply(&signal));
  assert(timing(7) - timing(5) == 800 && factor(0) == 0xccccd);
  assert((registers[0][0x32] & 0x13) == 0x11);
  video_blank(1);
  before = indirect_writes;
  video_set_aspect(0); /* Save preference during signal loss, without writes. */
  assert(indirect_writes == before);
  assert(video_apply(&signal));
  assert_picture(640);
}

static void forced_aspect_controls(void) {
  video_signal_t signal;
  assert(video_aspect_available(VIDEO_ASPECT_KEEP));
  assert(video_aspect_available(VIDEO_ASPECT_FILL));
#if !RTD_ASPECT_16_9
  assert(!video_aspect_available(VIDEO_ASPECT_16_9));
#endif
  assert(!video_aspect_available(255));
  fixture(800, 13714);
  assert(video_measure(&signal) && video_apply(&signal));
#if RTD_ASPECT_4_3
  {
    unsigned phase, tap;
    uint32_t clock = pll_estimate();
    assert(video_aspect_available(VIDEO_ASPECT_4_3));
    assert(downscale_coefficient_count == 64);
    for (phase = 0; phase < 8; ++phase) {
      unsigned sum = 0;
      for (tap = 0; tap < 4; ++tap) {
        unsigned index = 2 * (8 * tap + phase);
        sum += downscale_coefficients[index] |
               ((unsigned)downscale_coefficients[index + 1] << 8);
      }
      assert(sum == 1024);
    }
    video_set_aspect(VIDEO_ASPECT_4_3);
    assert_picture(640);
    assert(word(0x16) == 800); /* Preserve the entire source capture. */
    assert(ports[0][0x30][0] == 0x21 && ports[0][0x30][1] == 0x80);
    assert(ports[0][0x30][2] == 0xe0); /* FIFO receives 640x480 after UZD. */
    assert((registers[6][0xe3] & 0xbf) == 0x21);
    assert(!(registers[6][0xe4] & 0x3c));
    assert(registers[6][0xe5] == 0x14 && !registers[6][0xe6] &&
           !registers[6][0xe7]); /* 800/640 in Q20. */
    assert(!(registers[6][0xed] & 7) && !registers[6][0xee]);
    assert((registers[6][0xef] & 7) == 2 && registers[6][0xf0] == 0x80);
    assert(registers[6][0xf1] == 193);
    assert_vertical(525, 32);
    assert(pll_estimate() == clock);
#if !RTD_ASPECT_16_9
    video_set_aspect(VIDEO_ASPECT_16_9); /* Unsupported choice is a no-op. */
    assert_picture(640);
    assert(registers[6][0xe3] & 1);
#endif
    cvt_fixture(14501, 500);
    assert(video_measure(&signal) && video_apply(&signal));
    assert_picture(640);
    assert_vertical(500, 10);
    assert(registers[6][0xe3] & 1);
    fixture(640, 13714);
    assert(video_measure(&signal) && video_apply(&signal));
    assert_picture(640);
    assert(!(registers[6][0xe3] & 3)); /* VGA 4:3 is already native. */
    video_set_aspect(VIDEO_ASPECT_FILL);
    assert(timing(7) - timing(5) == 800 && factor(0) == 0xccccd);
    video_set_aspect(VIDEO_ASPECT_KEEP);
    assert_picture(640);
  }
#else
  assert(!video_aspect_available(VIDEO_ASPECT_4_3));
  video_set_aspect(VIDEO_ASPECT_4_3);
  assert_picture(800);
#endif
}

#if RTD_ASPECT_16_9
static void wide_aspect_controls(void) {
  video_signal_t signal;
  uint32_t source_clock, expected_clock, measured_clock;
  uint8_t source_width;
  video_blank(1);
  assert(!video_aspect_available(VIDEO_ASPECT_16_9));
  video_set_aspect(VIDEO_ASPECT_16_9); /* Restore a preference before input. */
  assert(video_aspect_current() == VIDEO_ASPECT_KEEP);
  for (source_width = 0; source_width < 2; ++source_width) {
    uint16_t htotal = source_width ? 1000 : 800;
    uint16_t frame_lines = source_width ? 2 : 5;
    uint16_t frame_clocks = source_width ? 40 : 44;
    uint32_t original_first, scaled_first, delta;
    fixture(source_width ? 800 : 640, 13714);
    assert(video_measure(&signal) && video_apply(&signal));
    assert(video_aspect_available(VIDEO_ASPECT_16_9));
    assert(video_aspect_current() == VIDEO_ASPECT_16_9);
    source_clock = signal.output_clock_hz;
    expected_clock = (uint32_t)((uint64_t)source_clock * 15 / 16);
    measured_clock = pll_estimate();
    assert((expected_clock > measured_clock ? expected_clock - measured_clock :
            measured_clock - expected_clock) < 1000);
    assert(timing(0x0b) == 493 && video_display_vstart() == 6);
    assert(timing(0x0e) == 6 && timing(0x14) == 486);
    assert(timing(0x10) == 21 && timing(0x12) == 471);
    assert(timing(7) - timing(5) == 800);
    assert(word(0x1a) == 480 && word(0x16) == signal.width);
    assert(ports[0][0x30][2] == 0xc2); /* 450 output rows, complete480capture. */
    assert((registers[6][0xe3] & 0xbf) == 0x22);
    assert((registers[6][0xe4] & 0x3c) == 0x08);
    assert(registers[6][0xe8] == 0x11 && registers[6][0xe9] == 0x11 &&
           registers[6][0xea] == 0x11);
    assert(registers[6][0xf2] == 239);
    assert(factor(0) == (source_width ? 0xfffff : 0xccccd));
    assert(factor(1) == 0xfffff && !(registers[0][0x32] & 2));
    /* First-pixel lead follows the calibrated unity mode plus the reference
     * UZD's quarter-line difference, within one16-clock register quantum. */
    original_first = (uint32_t)frame_lines * htotal + (frame_clocks + 1) * 16 +
                     (uint32_t)((32088ULL * htotal) / 1000);
    scaled_first = (uint32_t)registers[0][0x40] * htotal +
                   ((uint16_t)registers[0][0x41] + 1) * 16 +
                   (uint32_t)((21088ULL * htotal * 16) / 15000);
    delta = original_first + htotal / 4 - scaled_first;
    assert(delta < 16);
    cvt_fixture(14501, 500);
    assert(video_measure(&signal) && video_apply(&signal));
    assert(!video_aspect_available(VIDEO_ASPECT_16_9));
    assert(video_aspect_current() == VIDEO_ASPECT_KEEP);
    assert_picture(800);
    assert_vertical(500, 10);
    assert(!(registers[6][0xe3] & 3));
  }
  fixture(800, 13800); /* Accepted525timing, but16:9DPLL would be below29.5MHz. */
  assert(video_measure(&signal) && video_apply(&signal));
  assert(!video_aspect_available(VIDEO_ASPECT_16_9));
  assert(video_aspect_current() == VIDEO_ASPECT_KEEP);
  assert_picture(800);
  fixture(640, 13714);
  assert(video_measure(&signal) && video_apply(&signal));
  assert(video_aspect_current() == VIDEO_ASPECT_16_9);
  video_set_aspect(VIDEO_ASPECT_KEEP);
  assert(video_aspect_current() == VIDEO_ASPECT_KEEP);
  assert_picture(640);
  assert_vertical(525, 32);
  assert(registers[0][0x40] == 5 && registers[0][0x41] == 44);
  assert(!(registers[6][0xe3] & 3));
  video_set_aspect(VIDEO_ASPECT_16_9);
  video_blank(1);
  assert(video_aspect_current() == VIDEO_ASPECT_KEEP);
  assert(!video_aspect_available(VIDEO_ASPECT_16_9));
  assert_vertical(525, 32); /* Signal loss restores the known startup raster. */
  video_set_aspect(VIDEO_ASPECT_KEEP);
}
#endif

static int16_t color_coefficient(unsigned row, unsigned column) {
  unsigned high = color_coefficients[row][2 * column];
  unsigned low = color_coefficients[row][2 * column + 1];
  assert(high <= 1);
  return (int16_t)low - (high ? 256 : 0);
}

static void color_controls(void) {
  unsigned row, column, percent;
  unsigned before = other_page_writes;
  video_set_picture(50, 50);
  video_set_color(0, 50, 100, 50);
  assert(picture_coefficients[3] == 0);
  assert(picture_coefficients[4] == 128);
  assert(picture_coefficients[5] == 255);
  video_set_picture(75, 25); /* Picture changes retain independent RGB gains. */
  assert(picture_coefficients[0] == 191);
  assert(picture_coefficients[3] == 0);
  assert(picture_coefficients[4] == 64);
  assert(picture_coefficients[5] == 128);
  video_set_color(50, 50, 255, 50); /* Color changes retain picture settings. */
  assert(picture_coefficients[0] == 191);
  assert(picture_coefficients[3] == 64 && picture_coefficients[5] == 128);
  for (percent = 0; percent <= 100; ++percent) {
    video_set_color(50, 50, 50, (uint8_t)percent);
    assert((registers[0][0x62] & 0xfc) == (percent == 50 ? 0xd8 : 0xdc));
    assert(registers[0][0x64] & 0x40); /* Two-bit coefficient enlargement. */
    for (row = 0; row < 3; ++row) {
      int sum = 256; /* The sRGB matrix has an implicit identity diagonal. */
      for (column = 0; column < 3; ++column)
        sum += color_coefficient(row, column);
      assert(sum == 256); /* Every grey input keeps its luma and neutrality. */
      assert(color_offsets[row] == 0);
    }
    if (percent == 0) {
      /* A red/green/blue primary becomes the same luma on every channel. */
      static const int luma[] = {77, 150, 29};
      for (row = 0; row < 3; ++row)
        for (column = 0; column < 3; ++column)
          assert(4 * color_coefficient(row, column) +
                 (row == column ? 1024 : 0) == 4 * luma[column]);
    }
    if (percent == 50)
      for (row = 0; row < 3; ++row)
        for (column = 0; column < 3; ++column)
          assert(color_coefficient(row, column) == 0);
    if (percent == 100) {
      assert(color_coefficient(0, 0) == 179);
      assert(color_coefficient(0, 1) == -150);
      assert(color_coefficient(0, 2) == -29);
    }
  }
  video_set_color(50, 50, 50, 255);
  assert(color_coefficient(0, 0) == 179); /* Clamp, never wrap. */
  assert(other_page_writes == before); /* No receiver/audio/PLL changes. */
  video_set_color(50, 50, 50, 50);
  video_set_picture(50, 50);
  assert((registers[0][0x62] & 0x40) && (registers[0][0x64] & 0x40));
}

static int16_t filter_coefficient(unsigned tap, unsigned phase) {
  unsigned offset = 2 * (16 * tap + phase);
  unsigned encoded = coefficients[offset] | ((unsigned)coefficients[offset + 1] << 8);
  assert(encoded <= 4095);
  return encoded & 2048 ? (int16_t)encoded - 4096 : (int16_t)encoded;
}

static void sharpness_controls(void) {
  video_signal_t signal;
  unsigned percent, phase, tap, step;
  unsigned before;
  fixture(800, 13714);
  assert(video_measure(&signal) && video_apply(&signal));
  before = other_page_writes;
  for (percent = 0; percent <= 100; ++percent) {
    uint8_t previous = registers[0][0x35] & 0x22;
    uint8_t previous_scale = registers[0][0x32];
    unsigned previous_count = coefficient_count;
    video_set_sharpness((uint8_t)percent);
    assert(coefficient_count == previous_count); /* Setter cannot block DDC. */
    for (step = 0; step < 64; ++step) {
      assert((registers[0][0x35] & 0x22) == previous);
      assert(registers[0][0x32] == previous_scale);
      video_controls_service();
      assert(coefficient_count == 2 * (step + 1));
    }
    assert((registers[0][0x35] & 0x22) != previous);
    assert(!(registers[0][0x35] & 0x88)); /* No access port left enabled. */
    assert(coefficient_count == 128);
    for (phase = 0; phase < 16; ++phase) {
      int sum = 0;
      int32_t linear = 16 + 32 * phase;
      int32_t amount = (int)percent - 50;
      int32_t reference[4];
      reference[0] = -amount * linear / 200;
      reference[1] = linear + amount * (3 * linear - 1024) / 200;
      reference[3] = -amount * (1024 - linear) / 200;
      reference[2] = 1024 - reference[0] - reference[1] - reference[3];
      for (tap = 0; tap < 4; ++tap)
        assert(filter_coefficient(tap, phase) == reference[tap]);
      for (tap = 0; tap < 4; ++tap) sum += filter_coefficient(tap, phase);
      assert(sum == 1024); /* Preserve uniform fields at every setting/phase. */
      if (percent == 0) {
        for (tap = 0; tap < 4; ++tap) assert(filter_coefficient(tap, phase) >= 0);
        assert(filter_coefficient(0, phase) > 0);
      } else if (percent == 50) {
        assert(filter_coefficient(0, phase) == 0);
        assert(filter_coefficient(1, phase) == 16 + 32 * (int)phase);
        assert(filter_coefficient(2, phase) == 1008 - 32 * (int)phase);
        assert(filter_coefficient(3, phase) == 0);
      } else if (percent == 100) {
        assert(filter_coefficient(0, phase) < 0 && filter_coefficient(3, phase) < 0);
      }
    }
    assert(factor(0) == 0xfffff && factor(1) == 0xfffff);
    assert((registers[0][0x32] & 0x13) == (percent == 50 ? 0x10 : 0x11));
    assert(timing(7) - timing(5) == 800);
  }
  video_set_sharpness(255);
  for (step = 0; step < 17; ++step) video_controls_service();
  assert(coefficient_count == 34);
  /* A second request restarts the partial bank without exposing it. */
  {
    uint8_t previous = registers[0][0x35] & 0x22;
    video_set_sharpness(255);
    for (step = 0; step < 64; ++step) {
      assert((registers[0][0x35] & 0x22) == previous);
      video_controls_service();
      assert(coefficient_count == 2 * (step + 1));
    }
  }
  video_controls_service(); /* Completed uploads stay idle. */
  assert(coefficient_count == 128);
  assert(filter_coefficient(3, 0) == -252);
  assert(other_page_writes == before);
  video_set_sharpness(50);
  video_blank(1); /* Service must also finish during signal loss. */
  for (step = 0; step < 64; ++step) video_controls_service();
  assert(video_apply(&signal));
  assert_picture(800);
}

int main(void) {
  video_signal_t signal;
  unsigned phase;
  unsigned tap;
  unsigned sum;

  video_init();
  for (phase = 0; phase < 6; ++phase) assert(picture_coefficients[phase] == 128);
  assert(registers[2][0xa7] == 0x6f); /* UC-586 differential and R/B swaps. */
  assert(timing(0) + 4 == panel.htotal);
  assert(timing(5) + 10 == panel.hstart);
  assert(timing(7) - timing(5) == panel.width);
  assert(timing(0x12) - timing(0x10) == panel.height);
  assert(registers[0][0x29] == 6);
  assert((registers[0][0x28] & 0xa8) == 0xa0); /* Forced background, free-run. */
  assert(!(registers[0][0x28] & 0x14)); /* 24-bit single-port output. */
  assert(coefficient_count == 128);
  for (phase = 0; phase < 16; ++phase) {
    sum = 0;
    for (tap = 0; tap < 4; ++tap) {
      unsigned offset = 2 * (16 * tap + phase);
      sum += coefficients[offset] | ((unsigned)coefficients[offset + 1] << 8);
    }
    assert(sum == 1024); /* Every phase preserves a uniform image. */
  }

  fixture(800, 13714);
  assert(video_measure(&signal));
  assert(signal.error == VIDEO_OK && signal.detail[0] == 13714 &&
         signal.detail[1] == 524 && signal.detail[2] == 0);
  assert(signal.measured == (VIDEO_MEASURE_GEOMETRY | VIDEO_MEASURE_TIMING));
  assert(signal.input_width == 800 && signal.input_height == 480);
  assert(signal.htotal == 1000 && signal.vtotal == 524);
  assert(signal.line_hz == 432000000UL / 13714 && signal.polarity == 0);
  assert(signal.width == 800 && signal.height == 480);
  assert(signal.output_clock_hz == (uint32_t)(432000000000ULL / 13714));
  assert(video_apply(&signal));
  assert_picture(800);
  assert(word(0x14) == 86 && word(0x18) == 32);
  assert(word(0x16) == 800 && word(0x1a) == 480);
  assert(registers[0][0x16] & 8); /* Capture-width write preserves TMDS path. */
  assert(factor(0) == 0xfffff && factor(1) == 0xfffff);
  assert((registers[0][0x32] & 0x13) == 0x10);
  assert(registers[0][0x40] == 2 && registers[0][0x41] == 40);
  assert((registers[0][0x28] & 0xa8) == 0x88); /* Input video, frame sync. */
  video_blank(1);
  assert((registers[0][0x28] & 0xa8) == 0xa0); /* Signal loss needs no IVS. */
  video_blank(0);
  assert((registers[0][0x28] & 0xa8) == 0x88);

  fixture(640, 13714);
  measurement[0][3] = 13; /* The alternate 525-line endpoint also works. */
  assert(video_measure(&signal) && video_apply(&signal));
  assert(word(0x14) == 142 && word(0x18) == 35);
  assert(word(0x16) == 640);
  assert_picture(640);
  assert(registers[0][0x40] == 5 && registers[0][0x41] == 44);

  signal.output_clock_hz = UINT32_MAX;
  assert(!video_apply(&signal));
  unsupported_metadata();
  clock_range();
  cvt_profile();
  polarity_profiles();
  reject_cases();
  avmute_recovery();
  picture_controls();
  aspect_controls();
  color_controls();
  sharpness_controls();
  forced_aspect_controls();
#if RTD_ASPECT_16_9
  wide_aspect_controls();
#endif
  puts("video: panel encoding, scaling, mode validation and timeout checks passed");
  return 0;
}
