#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include "pico/multicore.h"

#include "3DO.h"
#include "MSC.h"
#include "CDFormat.h"

#define DATA_BUS_MASK (0xFF<<CDD0)

#define DATA_IN 0
#define DATA_OUT 1

#include "read.pio.h"
#ifdef HW_HS_FC
#include "write_hs_fc.pio.h"
#else
#include "write.pio.h"
#endif

extern bool readBlock(uint32_t start, uint16_t nb_block, uint16_t block_size, uint8_t *buffer);
extern bool readSubQChannel(uint8_t *buffer);
extern bool USBDriveEject(uint8_t dev_addr, bool eject);
extern bool block_is_ready();
extern bool cmd_is_ready(bool *success);
extern bool isAudioBlock(uint32_t start);
extern void wakeUpCDRom(uint8_t dev_addr, uint8_t lun);

extern void USB_reset(void);

extern bool onBootIso;

#define GET_BUS(A) (((A)>>CDD0)&0xFF)

extern cd_s currentImage;

typedef enum{
  SPIN_UP = 0x2,
  EJECT_DISC = 0x6,
  INJECT_DISC = 0x7,
  SET_MODE = 0x09,
  ABORT = 0x08,
  FLUSH = 0x0B,
  READ_DATA = 0x10,
  DATA_PATH_CHECK = 0x80,
  READ_ERROR = 0x82,
  READ_ID    = 0x83,
  READ_CAPACITY = 0x85,
  READ_SUB_Q = 0x87,
  READ_DISC_INFO = 0x8B,
  READ_TOC = 0x8C,
  READ_SESSION = 0x8D,
//Fixel added commands
  EXT_ID = 0x93,
  CHANGE_TOC = 0xC0,
  GET_TOC = 0xC1,
  GET_DESC = 0xC2,
  CLEAR_PLAYLIST = 0xC3,
  ADD_PLAYLIST = 0xC4,
  LAUNCH_PLAYLIST = 0xC5,

  GET_TOC_LIST = 0xD1,

  CREATE_FILE = 0xE0,
  OPEN_FILE = 0xE1,
  SEEK_FILE = 0xE2,
  READ_FILE_BYTE = 0xE3,
  WRITE_FILE_BYTE = 0xE4,
  CLOSE_FILE = 0xE5,
  WRITE_BUFFER_OFFSET = 0xE6,
  READ_BUFFER = 0xE7,

  UPDATE_ODE = 0xF0,
}CD_request_t;

/*
case 0xc0: dev_ode_changetoc(); break;
case 0xc1: dev_ode_gettoc(); break;
case 0xc2: dev_ode_getdesc(); break;
case 0xc3: dev_ode_clearpl(); break;
case 0xc4: dev_ode_addpl(); break;
case 0xc5: dev_ode_launchpl(); break;

case 0xd1: dev_ode_gettoclist(); break;


case 0xe0: dev_ode_createfile(); break;
case 0xe1: dev_ode_openfile(); break;
case 0xe2: dev_ode_seekfile(); break;
case 0xe3: dev_ode_readfile(); break;
case 0xe4: dev_ode_writefile(); break;
case 0xe5: dev_ode_closefile(); break;
case 0xe6: dev_ode_bufsend(); break;
case 0xe7: dev_ode_bufrecv(); break;

case 0xf0: dev_ode_startupdate(); break;
*/

typedef enum{
  TRAY_IN =	0x80,
  DISC_PRESENT = 0x40,
  SPINNING = 0x20,
  CHECK_ERROR = 0x10,
  DOUBLE_SPEED = 0x02,
  DISC_RDY = 0x01
}CD_status_t;

typedef enum{
  NO_ERROR = 0x0,
  SOFT_READ_RETRY = 0x1,
  SOFT_READ_CORRECTION = 0x2,
  NOT_READY = 0x3,
  NO_TOC = 0x4,
  HARD_READ = 0x5,
  SEEK_FAIL = 0x6,
  TRACKING_FAIL = 0x7,
  DRIVE_RAM_ERROR = 0x8,
  SELF_TEST_ERROR = 0x9,
  FOCUSING_FAIL = 0xA,
  SPINDLE_FAIL = 0xB,
  DATA_PATH_FAIL = 0xC,
  ILLEGAL_LBA = 0xD,
  ILLEGAL_CDB = 0xE,
  END_USER_TRACK = 0xF,
  ILLEGAL_MODE = 0x10,
  MEDIA_CHANGED = 0x11,
  POWER_OR_RESET_OCCURED = 0x12,
  DRIVE_ROM_FAIL = 0x13,
  ILLEGAL_CMD = 0x14,
  DISC_REMOVED = 0x15,
  HARDWARE_ERROR = 0x16,
  ILLEGAL_REQUEST = 0x17,
}CD_error_t;

typedef enum {
  CHAN_WRITE_STATUS = 0,
  CHAN_WRITE_DATA,
  CHAN_MAX
} DMA_chan_t;


dma_channel_config config[CHAN_MAX];
uint8_t sm[CHAN_MAX];
uint8_t sm_offset[CHAN_MAX];
int channel[CHAN_MAX];

int errorOnDisk = 0;


uint sm_read = -1;
uint instr_jmp_read;

uint instr_out;
uint instr_pull;
uint instr_set;
uint instr_jmp[CHAN_MAX];
uint instr_restart;

uint nbWord = 0;

static bool use_cdrom = false;

static bool ledState = false;

static bool canHandleReset = false;

static int pitch = 0;

static uint8_t errorCode = POWER_OR_RESET_OCCURED;
static uint8_t status = TRAY_IN | CHECK_ERROR | DISC_RDY;

void close_tray(bool close) {
  device_s *dev = NULL;
  bool has_usb = false;
  bool has_cd = false;
  for (int i = 0; i<CFG_TUH_DEVICE_MAX; i++) {
    device_s *curdev = getDeviceIndex(i);
    if (curdev->type == MSC_TYPE) {
      has_usb = true;
    }
    if (curdev->type == CD_TYPE) {
      has_cd = true;
    }
  }
  LOG_SATA("Ask to eject %d has_sub %d has_cd %d\n", close, has_usb, has_cd);
  //By default eject currentImage device
  if (currentImage.dev_addr != 0xFF) dev = getDevice(currentImage.dev_addr);
  //In case there is no currentImage, we might have an invalid CD
  if ((currentImage.dev_addr == 0xFF) && (has_cd)) {
    for (int i = 0; i<CFG_TUH_DEVICE_MAX; i++) {
      device_s *curdev = getDeviceIndex(i);
      if (curdev->type == CD_TYPE) {
        dev = curdev;
        break;
      }
    }
  }
  if (dev != NULL) {
    //If the currentImage device is a cd but we have usb key, do not eject it
    if ((dev->type == CD_TYPE) && (has_usb))
      dev = NULL;
  }
  //But In case we are on Boot menu and there is a CD, eject the CD
  if (onBootIso & has_cd) {
    //on Boot menu, if there is a CDROM, try to eject it.
    for (int i = 0; i<CFG_TUH_DEVICE_MAX; i++) {
      device_s *curdev = getDeviceIndex(i);
      if (curdev->type == CD_TYPE) {
        dev = curdev;
        break;
      }
    }
  }
  if (dev != NULL) {
    LOG_SATA("request Eject on dev %d, type %s\n", dev->dev_addr, (dev->type==CD_TYPE)?"CD":"USB");
    if (!USBDriveEject(dev->dev_addr, !close)) {
      LOG_SATA("Can not eject/inject\n");
      return;
    }
  }
  // status &= ~TRAY_IN;
  if (!onBootIso)
    mediaInterrupt();
}

void wait_out_of_reset() {
  while( !gpio_get(CDRST)) {
    gpio_put(CDMDCHG, 1); //Under reset
  }
  sleep_ms(500);
  gpio_put(CDMDCHG, 0); //Under reset
  sleep_ms(100);
  gpio_put(CDMDCHG, 1); //Under reset
}

void set3doCDReady(uint8_t dev_addr, bool on) {
  if (on && (currentImage.dev_addr == dev_addr)) {
    errorOnDisk = 0;
    switch(currentImage.format) {
      case 0x0:
        if (currentImage.hasOnlyAudio)
          LOG_SATA("Audio CD detected\n");
        else
          LOG_SATA("Data CD detected\n");
          //Need to check if compatible 3DO game
        break;
      case 0x20:
        LOG_SATA("Photo-CD detected\n");
        break;
      case 0xFF:
        LOG_SATA("CD-i detected\n");
        break;
    }
    status = TRAY_IN | DISC_RDY | DISC_PRESENT | SPINNING;
    errorCode = 0;
    status &= ~CHECK_ERROR;
  } else {
    if (on) LOG_SATA("Current Image is on dev addr %x but CD is %x\n", currentImage.dev_addr, dev_addr);
  }

}

void set3doDriveMounted(uint8_t dev_addr, bool on) {
  device_s *dev = getDevice(dev_addr);
  if ((currentImage.dev_addr == dev_addr) && (!on)) {
    LOG_SATA("set3doDriveMounted %d\n", on);
    if (currentImage.curDir != NULL) free(currentImage.curDir);
    if (currentImage.curPath != NULL) free(currentImage.curPath);
    currentImage.curDir = NULL;
    currentImage.curPath = NULL;
    currentImage.dev_addr = 0xFF;
    currentImage.lun = 0x0;
  }
}

void set3doDriveReady() {
}

void set3doDriveError() {
  LOG_SATA("Drive error\n");
  errorOnDisk = 1;
  errorCode = SOFT_READ_RETRY;
  status |= CHECK_ERROR;
  status &= ~DISC_RDY;
  status &= ~SPINNING;
  USB_reset();
}

bool is3doData() {
  return pio_sm_get_rx_fifo_level(pio0, sm_read) != 0;
}

uint32_t get3doData() {
  uint32_t val = 0x0;
  val = pio_sm_get_blocking(pio0, sm_read);
  return val;
}

void print_dma_ctrl(dma_channel_hw_t *channel) {
    uint32_t ctrl = channel->ctrl_trig;
    int rgsz = (ctrl & DMA_CH0_CTRL_TRIG_RING_SIZE_BITS) >> DMA_CH0_CTRL_TRIG_RING_SIZE_LSB;
    LOG_SATA("(%08x) ber %d rer %d wer %d busy %d trq %d cto %d rgsl %d rgsz %d inw %d inr %d sz %d hip %d en %d",
           (uint) ctrl,
           ctrl & DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_READ_ERROR_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS ? 1 : 0,
           (int) ((ctrl & DMA_CH0_CTRL_TRIG_TREQ_SEL_BITS) >> DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB),
           (int) ((ctrl & DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS) >> DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB),
           ctrl & DMA_CH0_CTRL_TRIG_RING_SEL_BITS ? 1 : 0,
           rgsz ? (1 << rgsz) : 0,
           ctrl & DMA_CH0_CTRL_TRIG_INCR_WRITE_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_INCR_READ_BITS ? 1 : 0,
           1 << ((ctrl & DMA_CH0_CTRL_TRIG_DATA_SIZE_BITS) >> DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB),
           ctrl & DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_EN_BITS ? 1 : 0);
}

void setDataDir(int value) {
  gpio_put(DIR_DATA, value);
  if (value == DATA_IN) {
      pio_sm_drain_tx_fifo(pio0, sm_read);
  }
}


void restartPio(uint8_t channel) {
  pio_sm_drain_tx_fifo(pio0, sm[channel]);
  pio_sm_restart(pio0, sm[channel]);
  pio_sm_exec(pio0, sm[channel], instr_jmp[channel]);
  pio_sm_set_enabled(pio0, sm[channel], true);
}

static void restartReadPio() {
  pio_sm_drain_tx_fifo(pio0, sm_read);
  pio_sm_restart(pio0, sm_read);
  pio_sm_exec(pio0, sm_read, instr_jmp_read);
  pio_sm_set_enabled(pio0, sm_read, true);
}

void startDMA(uint8_t access, uint8_t *buffer, uint32_t nbWord) {
  dma_channel_transfer_from_buffer_now(channel[access], buffer, nbWord);
}

void sendAnswer(uint8_t *buffer, uint32_t nbWord, uint8_t access) {
  restartPio(access);
  startDMA(access, buffer, nbWord);
  pio_sm_set_consecutive_pindirs(pio0, sm[access], CDD0, 8, true);
  setDataDir(DATA_OUT); //Output data
  gpio_put(CDSTEN + access, 0x0);

  dma_channel_wait_for_finish_blocking(access);
  while(!pio_sm_is_tx_fifo_empty(pio0, sm[access]));
  while (pio_sm_get_pc(pio0, sm[access]) != sm_offset[access]);
  gpio_put(CDSTEN + access, 0x1);
  setDataDir(DATA_IN); //input data
  pio_sm_set_consecutive_pindirs(pio0, sm[access], CDD0, 8, false);
  pio_sm_set_enabled(pio0, sm[access], false);
}

bool sendAnswerStatusMixed(uint8_t *buffer, uint32_t nbWord, uint8_t *buffer_status, uint8_t nbStatus, bool last, bool trace) {
  bool statusStarted = false;
  bool interrupted = false;
  absolute_time_t a,b,c,d,e, s = get_absolute_time();
  bool lastCDEN = gpio_get(CDEN);
  if (trace) a= get_absolute_time();
  restartPio(CHAN_WRITE_DATA);
  if (trace) b= get_absolute_time();
  startDMA(CHAN_WRITE_DATA, buffer, nbWord);
  if (trace) c= get_absolute_time();
  pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, true);
  if (trace)  d= get_absolute_time();
  setDataDir(DATA_OUT); //Output data
  gpio_put(CDDTEN, 0x0);
  if (trace) e= get_absolute_time();
  absolute_time_t start = get_absolute_time();
  bool canBeInterrupted = false;

  if (trace)
    LOG_SATA("a %lld, b %lld, c %lld, d %lld, e %lld\n", absolute_time_diff_us(s,a), absolute_time_diff_us(a,b), absolute_time_diff_us(b,c), absolute_time_diff_us(c,d), absolute_time_diff_us(d,e));

  while (dma_channel_is_busy(CHAN_WRITE_DATA)) {
    if (gpio_get(CDEN) != lastCDEN) {
        lastCDEN = !lastCDEN;
        pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, !lastCDEN);
    }
    if((absolute_time_diff_us(start,get_absolute_time()) > 100) && (!statusStarted) && canBeInterrupted && last) {
      statusStarted = true;
      setDataDir(DATA_OUT);
      gpio_put(CDSTEN, 0x0);
    }

    if (!gpio_get(CDEN)) {
      start = get_absolute_time();
      canBeInterrupted = true;
    }

    if (!gpio_get(CDHWR))
    {
      pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], false);
      pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, false);
      gpio_put(CDDTEN, true);
      setDataDir(DATA_IN); //Input data
      return false;
    }

    if (gpio_get(CDEN) && statusStarted) {
      interrupted = true;
      pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], false);
      sendAnswer(buffer_status, nbStatus, CHAN_WRITE_STATUS);
      pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, true);
      pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], true);
    }
  }
  while(!pio_sm_is_tx_fifo_empty(pio0, sm[CHAN_WRITE_DATA]));
  while (pio_sm_get_pc(pio0, sm[CHAN_WRITE_DATA]) != sm_offset[CHAN_WRITE_DATA]);
  gpio_put(CDDTEN, 0x1);
  setDataDir(DATA_IN); //Input data
  if (!interrupted && last) sendAnswer(buffer_status, nbStatus, CHAN_WRITE_STATUS);
  else pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, false);
  pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], false);
  return true;
}

static uint8_t TOC[2048] = {0};
uint8_t FILE_BUFFER[2048] = {0};

static void handleTocChange(int index) {
  toc_entry toc;
  //switch to the right TOC level
  setTocLevel(index);
}

char* getPathForTOC(int entry) {
  if (getTocEntry(entry)==0x0) {
    device_s *dev = getDevice(entry>>24);
    if (!dev->useable) return NULL;
    char* result = malloc(7);
    snprintf(result, 7, "%d://", (entry>>24)-1);
    return result;
  } else {
    for (int i=0; i<2048;) {
      uint32_t flags = (TOC[i++]<<24)|(TOC[i++]<<16)|(TOC[i++]<<8)|(TOC[i++]<<0);
      if (flags != TOC_FLAG_INVALID) {
        uint32_t toc_id = (TOC[i++]<<24)|(TOC[i++]<<16)|(TOC[i++]<<8)|(TOC[i++]<<0);
        uint32_t name_length = (TOC[i++]<<24)|(TOC[i++]<<16)|(TOC[i++]<<8)|(TOC[i++]<<0);
        if (toc_id == entry) {
          if (flags != TOC_FLAG_FILE) return NULL;
          int pathLength = name_length + 1 + strlen(currentImage.curPath)+1;
          char* result = malloc(pathLength);
          snprintf(result, pathLength, "%s\\%s", currentImage.curPath, &TOC[i]);
          return result;
        }
        i += name_length;
      } else {
        return NULL;
      }
    }
  }
  return NULL;
}

void getTocFull(int index, int nb) {
  int toclen = 0;
  int id = 0;
  bool ended = false;
  memset(TOC,0xff,sizeof(TOC));
  if ((getTocLevel() == 0) && (getDeviceCount()>1)) {
    int max = CFG_TUH_DEVICE_MAX;
    if (nb<CFG_TUH_DEVICE_MAX) max = nb;
    int nbUsb = 0;
    int nbCD = 0;
    for (int i=0; i<max; i++) {
      device_s *dev = getDeviceIndex(i);
      if (dev->type != UNKNOWN_TYPE) {
        toc_entry *te = malloc(sizeof(toc_entry));
        memset(te, 0x0, sizeof(toc_entry));
        char name[20];
        if (dev->type == CD_TYPE) {
          snprintf(name, 20, "CD %d", nbCD);
          nbCD++;
        }
        if (dev->type == MSC_TYPE) {
          snprintf(name, 20, "USB %d", nbUsb);
          nbUsb++;
        }
        te->flags = (dev->isFatFs)?TOC_FLAG_DIR:TOC_FLAG_FILE;
        te->toc_id = dev->dev_addr<<24;
        te->name_length = strlen(name) + 1;
        te->name = malloc(te->name_length);
        snprintf(te->name, te->name_length, "%s", name);
        te->name[te->name_length-1] = 0;

        TOC[toclen++]=te->flags>>24;
        TOC[toclen++]=te->flags>>16;
        TOC[toclen++]=te->flags>>8;
        TOC[toclen++]=te->flags&0xff;
        TOC[toclen++]=te->toc_id>>24;
        TOC[toclen++]=te->toc_id>>16;
        TOC[toclen++]=te->toc_id>>8;
        TOC[toclen++]=te->toc_id&0xff;
        TOC[toclen++]=te->name_length>>24;
        TOC[toclen++]=te->name_length>>16;
        TOC[toclen++]=te->name_length>>8;
        TOC[toclen++]=te->name_length&0xff;
        for(uint32_t t=0;t<te->name_length;t++) {
          TOC[toclen++]=te->name[t];
        }
        if (te->name != NULL) free(te->name);
        free(te);
      }

    }
  } else {
    if (!seekTocTo(index)) {
      LOG_SATA("Index %d is out of files number\n", index);
      return;
    }
    while(!ended) {
      toc_entry *te = malloc(sizeof(toc_entry));
      memset(te, 0x0, sizeof(toc_entry));
      if ((index == 0) && (id == 0) && (getTocLevel() != 0)) {
        if (!getReturnTocEntry(te)) break;
      } else {
        if (!getNextTOCEntry(te)) break;
      }
      id++;
      if ((nb != -1) && (id >= (nb))) {
        LOG_SATA("Limit has been reached on Id %d\n", id, nb);
        ended = true;
      } else {
        LOG_SATA("Got %d files on %d\n", id, nb+index);
      }
      te->toc_id &= ~(0xFF<<24);
      te->toc_id |= getTocIndex()<<24;
      TOC[toclen++]=te->flags>>24;
      TOC[toclen++]=te->flags>>16;
      TOC[toclen++]=te->flags>>8;
      TOC[toclen++]=te->flags&0xff;
      TOC[toclen++]=te->toc_id>>24;
      TOC[toclen++]=te->toc_id>>16;
      TOC[toclen++]=te->toc_id>>8;
      TOC[toclen++]=te->toc_id&0xff;
      TOC[toclen++]=te->name_length>>24;
      TOC[toclen++]=te->name_length>>16;
      TOC[toclen++]=te->name_length>>8;
      TOC[toclen++]=te->name_length&0xff;
      for(uint32_t t=0;t<te->name_length;t++) {
        TOC[toclen++]=te->name[t];
      }
      if (te->name != NULL) free(te->name);
      free(te);
      if (toclen > (sizeof(TOC)-(128+13))) {
        LOG_SATA("Buffer is full\n");
        ended = true;
      }
    }

  }

}

void getToc(int index, int offset, uint8_t* buffer) {
  if (offset == 0) {
    //init the TOC buffer
    getTocFull(index, -1);
  }
  memcpy(buffer, &TOC[offset], 16);

}

absolute_time_t lastPacket;
void sendData(int startlba, int nb_block, bool trace) {

  uint8_t buffer[2500];
  uint8_t status_buffer[2] = {READ_DATA, status};
  int start = startlba;
  absolute_time_t a,b,c,d,e, s;
  int reste = 0;

  if (nb_block == 0) return;
  int id = 0;
  while (nb_block != 0) {
    s = get_absolute_time();
    int current = id;
    int data_idx = 0;
    if (trace) a= get_absolute_time();
    if (!(status & DISC_PRESENT)) {
      return;
    }

    readBlock(startlba, 1, currentImage.block_size_read, &buffer[0]);
    if (trace) b= get_absolute_time();
    while(!block_is_ready() && !errorOnDisk && gpio_get(CDRST));

    if (!gpio_get(CDRST)) return;
    if (isAudioBlock(startlba)) {
      int timeForASecond = (990 + pitch) * 1000 + reste;  //Shall be 1s but cd drive is a bit slower than expected
      int correctedDelay = timeForASecond/75;
      reste = timeForASecond % 75;
      if (is_nil_time(lastPacket)) {
        lastPacket = delayed_by_us(get_absolute_time(),correctedDelay);
      } else {
        absolute_time_t currentPacket = get_absolute_time();
        int64_t delay = absolute_time_diff_us(currentPacket, lastPacket); /*Right number shall be 1000000/75*/
        if (delay>0) sleep_us(delay);
        else lastPacket = currentPacket;
        lastPacket = delayed_by_us(lastPacket,correctedDelay);
      }
    }
    if (trace) c = get_absolute_time();
    nb_block--;
    startlba++;
    id = (id++)%2;
    if (trace) d= get_absolute_time();
    if (!sendAnswerStatusMixed(&buffer[0], currentImage.block_size_read, status_buffer, 2, nb_block == 0, trace)) return;
    if (trace) e = get_absolute_time();
    if (trace)
      LOG_SATA("send data a %lld, b %lld, c %lld, d %lld, e %lld\n", absolute_time_diff_us(s,a), absolute_time_diff_us(a,b), absolute_time_diff_us(b,c), absolute_time_diff_us(c,d), absolute_time_diff_us(d,e));

  }
}

void sendRawData(int command, uint8_t *buffer, int length) {
  uint8_t status_buffer[2] = {command, status};
  if (length == 0) return;
  if (!sendAnswerStatusMixed(buffer, length, status_buffer, 2, true, false)) return;
}

static bool hasMediaInterrupt = false;
void mediaInterrupt(void) {
  hasMediaInterrupt = true;
  // canHandleReset = false;
  // status |= TRAY_IN;
}
void handleMediaInterrupt() {
  if (!hasMediaInterrupt) return;
  if ((status & DISC_PRESENT) && !canHandleReset) {
    gpio_put(LED, ledState);
    ledState = !ledState;
    return;
  }
  hasMediaInterrupt = false;
  // gpio_set_dir(CDRST, true);
  // gpio_put(CDRST, 0);
  // pio_sm_set_enabled(pio0, sm_read, false);
  // gpio_put(CDMDCHG, 1); //Under reset
  // sleep_ms(200);
  // gpio_put(CDRST, 1);
  // gpio_set_dir(CDRST, false);
  // sleep_ms(150);
  // gpio_put(CDMDCHG, 0); //Under reset
  // sleep_ms(10);
  gpio_put(CDMDCHG, 0); //Under reset
  status &= ~DISC_PRESENT & ~SPINNING;
  status |= CHECK_ERROR;
  errorCode = DISC_REMOVED;
  sleep_ms(10);
  while (!pio_sm_is_rx_fifo_empty(pio0, sm_read)) {
    pio_sm_get(pio0, sm_read);
  }
  LOG_SATA("Look for BootImage\n");
  onBootIso = false;
  if (getPlaylistEntries() == 0) {
    currentImage.dev_addr = 0xFF;
    currentImage.lun = 0x0;
  }
  if (currentImage.dev_addr != 0xFF) {
    device_s *current_dev = getDevice(currentImage.dev_addr);
    if (current_dev->type == CD_TYPE) {
      currentImage.dev_addr = 0xFF;
      currentImage.lun = 0x0;
    }
  }
  for (int i=0; i<CFG_TUH_DEVICE_MAX; i++) {
    LOG_SATA("Look for BootImage on %d\n", i);
    device_s *dev = getDeviceIndex(i);
    if (dev->dev_addr == 0xFF) {
      LOG_SATA("%d is not a real device\n", i);
      continue;
    }
    if (dev->type == MSC_TYPE) {
      LOG_SATA("%d is a MSC\n", i);
      requestBootImage();
      waitForLoad();
      LOG_SATA("Loaded on %d\n", i);
    }
    if (dev->type == CD_TYPE) {
      LOG_SATA("%d is a CD (%d)\n", i, dev->state);
      wakeUpCDRom(dev->dev_addr, dev->lun);
      set3doCDReady(dev->dev_addr, dev->state == MOUNTED);
      set3doDriveMounted(dev->dev_addr, dev->state  == MOUNTED);
    }

  }
  gpio_put(CDMDCHG, 1); //Under reset
  // pio_sm_set_enabled(pio0, sm_read, false);
  // errorCode |= POWER_OR_RESET_OCCURED;
}

void handleCommand(uint32_t data) {
  CD_request_t request = (CD_request_t) GET_BUS(data);
  bool isCmd = (data>>CDCMD)&0x1 == 0x0;
  uint32_t data_in[6];
  uint8_t buffer[18];
  uint8_t subBuffer[16];
  uint index = 0;

  gpio_put(LED, ledState);
  ledState = !ledState;

  switch(request) {
    case READ_ID:
    for (int i=0; i<6; i++) {
      data_in[i] = get3doData();
    }

    LOG_SATA("READ ID %x\n", status);
      if (errorOnDisk != 0) errorOnDisk++;
      buffer[index++] = READ_ID;
      buffer[index++] =0x00; //manufacture Id
      buffer[index++] =0x10;
      buffer[index++] =0x00; //manufacture number
      buffer[index++] =0x01;
      buffer[index++] =0x30;
      buffer[index++] =0x75;
      buffer[index++] =0x00; //revision number
      buffer[index++] =0x00;
      buffer[index++] =0x00; //flag
      buffer[index++] =0x00;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case EJECT_DISC:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      LOG_SATA("EJECT\n");
      canHandleReset = true;
      close_tray(false);
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case INJECT_DISC:
        for (int i=0; i<6; i++) {
          data_in[i] = get3doData();
        }
        LOG_SATA("INJECT\n");
        close_tray(true);
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        break;
    case READ_ERROR:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      LOG_SATA("READ ERROR %x %x\n", errorCode, status);
      buffer[index++] = READ_ERROR;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = errorCode; //error code
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      status &= ~CHECK_ERROR;
      errorCode = NO_ERROR;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case DATA_PATH_CHECK:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      LOG_SATA("DATA_PATH_CHECK %d\n", (status & DISC_PRESENT));
      buffer[index++] = DATA_PATH_CHECK;
      if (!(status & DISC_PRESENT)) {
        buffer[index++] = 0xA5;
        buffer[index++] = 0xA5;
      } else {
        buffer[index++] = 0xAA; //This means ok
        buffer[index++] = 0x55; //This means ok. Not the case when no disc
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      canHandleReset = true;
      break;
    case SPIN_UP:
      if (status & DISC_PRESENT) {
        for (int i=0; i<6; i++) {
          data_in[i] = get3doData();
        }
        LOG_SATA("SPIN UP\n");
        buffer[index++] = SPIN_UP;
        if (!(status & TRAY_IN)) {
            status |= CHECK_ERROR;
            errorCode |= ILLEGAL_CMD;
        } else {
          if (!(status & DISC_RDY)) {
            status |= CHECK_ERROR;
            errorCode = DISC_REMOVED;
          } else {
            status |= SPINNING;
          }
        }
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      }
      break;
    case READ_DATA:
      if (status & DISC_PRESENT) {
        for (int i=0; i<6; i++) {
          data_in[i] = GET_BUS(get3doData());
        }
        LOG_SATA("READ DATA MSF %d:%d:%d %x\n", data_in[0], data_in[1], data_in[2], status);
        if (data_in[3] == 0x00) {
          //MSF
          int lba = data_in[0]*60*75+data_in[1]*75+data_in[2] - 150;
          int nb_block = (data_in[4]<<8)|data_in[5];
          sendData(lba, nb_block, false);
        } else {
          //LBA not supported yet
          LOG_SATA("LBA not supported yet\n");
        }
      }
      break;
    case ABORT:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      LOG_SATA("ABORT\n");
      buffer[index++] = ABORT;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case FLUSH:
        for (int i=0; i<6; i++) {
          data_in[i] = get3doData();
        }
        LOG_SATA("FLUSH\n");
        buffer[index++] = FLUSH;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        break;
    case READ_DISC_INFO:
      if (status & DISC_PRESENT) {
        for (int i=0; i<6; i++) {
          data_in[i] = get3doData();
        }
        LOG_SATA("DISC_INFO\n");
        buffer[index++] = READ_DISC_INFO;
        // LBA = (((M*60)+S)*75+F)-150
        if (status & DISC_PRESENT) {
          buffer[index++] = currentImage.format;
          buffer[index++] = currentImage.first_track;
          buffer[index++] = currentImage.last_track;
          buffer[index++] = currentImage.msf[0];
          buffer[index++] = currentImage.msf[1];
          buffer[index++] = currentImage.msf[2];

        } else {
          errorCode = NOT_READY;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
        }
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        LOG_SATA("%d %x %d %d %d:%d:%d\n", (status & DISC_PRESENT), buffer[1], buffer[2],buffer[3],buffer[4],buffer[5],buffer[6]);
      }
      break;
    case READ_TOC:
      if (status & DISC_PRESENT) {
        for (int i=0; i<6; i++) {
          data_in[i] = GET_BUS(get3doData());
        }
        LOG_SATA("READ_TOC %x\n", data_in[1]);
        buffer[index++] = READ_TOC;
        buffer[index++] = 0x0; //NixByte?
        buffer[index++] = currentImage.tracks[data_in[1]-1].CTRL_ADR; //ADDR
        buffer[index++] = currentImage.tracks[data_in[1]-1].id; //ENT_NUMBER
        buffer[index++] = 0x0;//Format
        buffer[index++] = currentImage.tracks[data_in[1]-1].msf[0];
        buffer[index++] = currentImage.tracks[data_in[1]-1].msf[1];
        buffer[index++] = currentImage.tracks[data_in[1]-1].msf[2];
        buffer[index++] = 0x0;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        LOG_SATA("%x %d %d:%d:%d\n", buffer[2], buffer[3],buffer[5],buffer[6],buffer[7]);
      }
      break;
    case READ_SESSION:
      if (status & DISC_PRESENT) {
        for (int i=0; i<6; i++) {
          data_in[i] = get3doData();
        }
        LOG_SATA("READ_SESSION\n");
        buffer[index++] = READ_SESSION;
        if (currentImage.multiSession) {
          //TBD with a multisession disc
          buffer[index++] = 0x80;
          buffer[index++] = currentImage.msf[0]; //might some other values like msf for multisession start
          buffer[index++] = currentImage.msf[1];
          buffer[index++] = currentImage.msf[2];
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
        } else {
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
        }
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        LOG_SATA("%d:%d:%d\n", buffer[2], buffer[3],buffer[4]);
      }
    break;
    case READ_CAPACITY:
      if (status & DISC_PRESENT) {
        for (int i=0; i<6; i++) {
          data_in[i] = get3doData();
        }
        LOG_SATA("READ_CAPACITY\n");
        buffer[index++] = READ_CAPACITY;
        buffer[index++] = currentImage.msf[0];
        buffer[index++] = currentImage.msf[1];
        buffer[index++] = currentImage.msf[2];
        buffer[index++] = 0x0; //0x8?
        buffer[index++] = 0x0;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      }
      break;
      case SET_MODE:
        for (int i=0; i<6; i++) {
          data_in[i] = GET_BUS(get3doData());
        }
        LOG_SATA("SET_MODE %x %x %x %x %x %x\n", data_in[0], data_in[1], data_in[2], data_in[3], data_in[4], data_in[5]);
        /*
        not entirely full
[18:04]
cmd9 has many settings
[18:04]
[0] = 0x9
[18:04]
[1] = setting type
[18:04]
where :
[18:04]
0 = density
[18:04]
1 = error recovery
[18:04]
2 = stop time
[18:05]
3 = speed+pitch
[18:05]
4 = chunk size
[18:06]
density : [2] = density code, [3][4] = block length [5] = flags
[18:06]
error recovery : [2] = type [3] = retry count
[18:07]
stop time : [2] = time in 100ms
[18:07]
speed/pitch: [2]=speed [3][4] = pitch
[18:09]
chunk size : [2] = chunk size (1-8)

fixel — Aujourd’hui à 18:17
on density : [5] flags :
[18:18]
0x80 = 2353 bytes CDDA + error correction (modifié)
[18:19]
0x40 = 2448 bytes CDDA+subcode
[18:19]
0xc0 (0x80|0x40) = 2449 bytes : CDDA+subcode+error correction
[18:19]
0x00 : 2048
[18:21]
what's your reply to 0x83?
*/

        if (data_in[0] == 0x3) {
          if (data_in[1] & (0x80)) {
            status |= DOUBLE_SPEED;
          } else {
            status &= ~DOUBLE_SPEED;
          }

          if (data_in[2] & 0x4) {
            //Pitch correction On
            pitch = ((data_in[2]&0x3)<<8) | data_in[3];
            if (data_in[2]&0x2) pitch -= 1024;
          } else {
            pitch = 0;
          }
        }
        if (data_in[0] == 0x0) {
          currentImage.block_size_read = (data_in[2]<<8)|(data_in[3]);
          if ((data_in[4] & 0xC0) == 0x80) {
            currentImage.block_size_read = 2353; //CDDA + error correction
          }
          if ((data_in[4] & 0xC0) == 0x40) {
            currentImage.block_size_read = 2448; //CDDA+subcode
          }
          if ((data_in[4] & 0xC0) == 0xC0) {
            currentImage.block_size_read = 2449; //CDDA+subcode+error correction
          }
          LOG_SATA("Block size to %d\n",currentImage.block_size_read );
        }
        buffer[index++] = SET_MODE;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        break;
    case READ_SUB_Q:
    if (status & DISC_PRESENT) {
        for (int i=0; i<6; i++) {
          data_in[i] = GET_BUS(get3doData());
        }
        LOG_SATA("READ_SUB_Q\n");
        buffer[index++] = READ_SUB_Q;
        readSubQChannel(&subBuffer[0]);
        while(!block_is_ready());

        buffer[index++] = subBuffer[5]; //Ctrl/adr
        buffer[index++] = subBuffer[6]; //trk
        buffer[index++] = subBuffer[7]; //idx
        buffer[index++] = 0;
        buffer[index++] = subBuffer[9]; //total M
        buffer[index++] = subBuffer[10]; //total S
        buffer[index++] = subBuffer[11]; //total F
        buffer[index++] = subBuffer[13]; //current M
        buffer[index++] = subBuffer[14]; //current S
        buffer[index++] = subBuffer[15]; //current F
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      }
        break;
//FIXEL Extended commands implementation for ODE menu
    case EXT_ID:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      LOG_SATA("EXT_ID\n");
      buffer[index++] = EXT_ID;
      buffer[index++] = 0x1; //Where is located the boot.iso microsd=1, usbmsc=2, flash=4, sata=8
      buffer[index++] = 'L';
      buffer[index++] = 'O';
      buffer[index++] = 'C';
      buffer[index++] = 'O';
      buffer[index++] = 'D';
      buffer[index++] = 'E';
      buffer[index++] = 1; //rev major
      buffer[index++] = 1; //rev minor
      buffer[index++] = 0; //rev patch
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case CHANGE_TOC:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      buffer[index++] = CHANGE_TOC;
      {
        uint32_t entry = 0;
        entry = ((data_in[0]&0xFF)<<24)|((data_in[1]&0xFF)<<16)|((data_in[2]&0xFF)<<8)|((data_in[3]&0xFF)<<0);
        LOG_SATA("CHANGE_TOC %d on %d\n", entry&0xFFFFFF, entry>>24);
        handleTocChange(entry);
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case GET_TOC:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      buffer[index++] = GET_TOC;
      {
        uint32_t entry = 0;
        uint16_t offset = 0;
        entry = ((data_in[0]&0xFF)<<24)|((data_in[1]&0xFF)<<16)|((data_in[2]&0xFF)<<8)|((data_in[3]&0xFF)<<0);
        offset = ((data_in[4]&0xFF)<<8)|((data_in[5]&0xFF)<<0);
        LOG_SATA("GET_TOC %d %d\n", entry, offset);
        getToc(entry, offset, &buffer[index]);
        for (int i=0; i<16; i++) LOG_SATA("%x ", buffer[index+i]);
        LOG_SATA("\n");
        index += 16;
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case GET_DESC:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      LOG_SATA("GET_DESC\n");
      buffer[index++] = GET_DESC;
      buffer[index++] = status;
      break;
    case CLEAR_PLAYLIST:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      LOG_SATA("CLEAR_PLAYLIST\n");
      buffer[index++] = CLEAR_PLAYLIST;
      buffer[index++] = status;
      clearPlaylist();
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case ADD_PLAYLIST:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      buffer[index++] = ADD_PLAYLIST;
      {
        uint32_t entry = 0;
        bool valid = false;
        bool added = false;
        entry = ((data_in[0]&0xFF)<<24)|((data_in[1]&0xFF)<<16)|((data_in[2]&0xFF)<<8)|((data_in[3]&0xFF)<<0);
        LOG_SATA("ADD_PLAYLIST %d\n", entry);
        addToPlaylist(entry, &valid, &added);
        buffer[index++] = valid;
        buffer[index++] = added;
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case LAUNCH_PLAYLIST:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      LOG_SATA("LAUNCH_PLAYLIST\n");
      //Initiate an media change error
      status |= CHECK_ERROR;
      buffer[index++] = LAUNCH_PLAYLIST;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      mediaInterrupt();
      break;
    case GET_TOC_LIST:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      buffer[index++] = GET_TOC_LIST;
      {
        uint32_t entry = 0;
        uint16_t offset = 0;
        entry = ((data_in[0]&0xFF)<<24)|((data_in[1]&0xFF)<<16)|((data_in[2]&0xFF)<<8)|((data_in[3]&0xFF)<<0);
        offset = ((data_in[4]&0xFF)<<8)|((data_in[5]&0xFF)<<0);
        LOG_SATA("GET_TOC_LIST %d %d\n", entry, offset);
        getTocFull(entry, offset);
      }
      sendRawData(GET_TOC_LIST, &TOC[0], 2048);
      LOG_SATA("TOC Buffer:\n");
      for (int i=0; i<2048;) {
        uint32_t flags = (TOC[i++]<<24)|(TOC[i++]<<16)|(TOC[i++]<<8)|(TOC[i++]<<0);
        if (flags != TOC_FLAG_INVALID) {
          uint32_t toc_id = (TOC[i++]<<24)|(TOC[i++]<<16)|(TOC[i++]<<8)|(TOC[i++]<<0);
          uint32_t name_length = (TOC[i++]<<24)|(TOC[i++]<<16)|(TOC[i++]<<8)|(TOC[i++]<<0);
          LOG_SATA(".flags :0x%x, .toc_id: %d %d, .name_length: %d, .name: %s\n", flags, toc_id>>24, getTocEntry(toc_id), name_length, &TOC[i]);
          i += name_length;
        } else break;
      }
      LOG_SATA("\n");
      break;
    //Not supported yet
    case CREATE_FILE:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      LOG_SATA("CREATE_FILE\n");
      buffer[index++] = CREATE_FILE;
      buffer[index++] = 0; //Always report a failure
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case OPEN_FILE:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      {
        bool write = (data_in[5]!=0);
        bool success = false;
        if ((data_in[0]&0xFF) != 0) {
          uint32_t toc_id = ((data_in[1]&0xFF)<<24)|((data_in[2]&0xFF)<<16)|((data_in[3]&0xFF)<<8)|((data_in[4]&0xFF)<<0);
          LOG_SATA("OPEN_FILE TOC_ID %x, W/R %d\n", toc_id, write);
        } else {
          uint16_t name_length = ((data_in[1]&0xFF)<<8)|((data_in[2]&0xFF)<<0);
          LOG_SATA("OPEN_FILE NAME %d %s, W/R %d\n", name_length, &FILE_BUFFER[0], write);
          requestOpenFile(&FILE_BUFFER[0], name_length, write);
          while (!cmd_is_ready(&success));
        }
        buffer[index++] = OPEN_FILE;
        buffer[index++] = success; //Always report a failure
        buffer[index++] = status;
      }
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case SEEK_FILE:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      LOG_SATA("SEEK_FILE\n");
      buffer[index++] = SEEK_FILE;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case READ_FILE_BYTE:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      {
        bool success = false;
        uint16_t length = ((data_in[0]&0xFF)<<8)|((data_in[1]&0xFF)<<0);
        LOG_SATA("READ_FILE_BYTE %x\n", length);
        requestReadFile(&FILE_BUFFER[0], length);
        while (!cmd_is_ready(&success));
        buffer[index++] = READ_FILE_BYTE;
        buffer[index++] = success?data_in[0]:0;
        buffer[index++] = success?data_in[1]:0;
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case WRITE_FILE_BYTE:
    for (int i=0; i<6; i++) {
      data_in[i] = GET_BUS(get3doData());
    }{
      bool success = false;
      uint16_t length = ((data_in[0]&0xFF)<<8)|((data_in[1]&0xFF)<<0);
      LOG_SATA("WRITE_FILE_BYTE %x\n", length);
      requestWriteFile(&FILE_BUFFER[0], length);
      while (!cmd_is_ready(&success));
      buffer[index++] = WRITE_FILE_BYTE;
      buffer[index++] = success?data_in[0]:0;
      buffer[index++] = success?data_in[1]:0;
    }
    buffer[index++] = status;
    sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case CLOSE_FILE:
    for (int i=0; i<6; i++) {
      data_in[i] = GET_BUS(get3doData());
    }
    LOG_SATA("CLOSE_FILE\n");
    requestCloseFile();
    while (!cmd_is_ready(NULL));
    buffer[index++] = CLOSE_FILE;
    buffer[index++] = status;
    sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case WRITE_BUFFER_OFFSET:
    for (int i=0; i<6; i++) {
      data_in[i] = GET_BUS(get3doData());
    }
    {
      uint16_t offset = (((data_in[0]&0xFF)<<8)|((data_in[1]&0xFF)<<0))<<2;
      LOG_SATA("WRITE_BUFFER_OFFSET 0x%x\n", offset);
      for (int i=0; i<4; i++) {
        FILE_BUFFER[offset+i] = data_in[2+i];
      }
    }
    buffer[index++] = WRITE_BUFFER_OFFSET;
    buffer[index++] = status;
    sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case READ_BUFFER:
    for (int i=0; i<6; i++) {
      data_in[i] = GET_BUS(get3doData());
    }
    LOG_SATA("READ_BUFFER\n");
    sendRawData(READ_BUFFER, &FILE_BUFFER[0], 2048);
      break;
    case UPDATE_ODE:
    for (int i=0; i<6; i++) {
      data_in[i] = GET_BUS(get3doData());
    }
    LOG_SATA("UPDATE_ODE\n");
    buffer[index++] = UPDATE_ODE;
    buffer[index++] = status;
    sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    default: LOG_SATA("unknown Cmd %x\n", request);
  }
}

void setupDMA(uint8_t access) {
  channel[access] = dma_claim_unused_channel(true);
  config[access] = dma_channel_get_default_config(channel[access]);
  channel_config_set_transfer_data_size(&config[access], DMA_SIZE_8);
  channel_config_set_read_increment(&config[access], true);
  channel_config_set_write_increment(&config[access], false);
  channel_config_set_irq_quiet(&config[access], true);
  channel_config_set_dreq(&config[access], DREQ_PIO0_TX0 + access);
  dma_channel_set_write_addr(channel[access], &pio0->txf[sm[access]], false);
  dma_channel_set_config(channel[access], &config[access], false);

  bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
}

void pio_program_init(int channel) {
  sm[channel] = channel;
  sm_offset[channel] = pio_add_program(pio0, &write_program);
  write_program_init(pio0, sm[channel], sm_offset[channel]);
  instr_jmp[channel] = pio_encode_jmp(sm_offset[channel]);
  LOG_SATA("Prog %d offset %d\n", channel, sm_offset[channel]);
}

static absolute_time_t s;
static bool debounceEject = false;

void core1_entry() {

  uint32_t data_in;
  bool reset_occured = true;
  bool ejectState = gpio_get(EJECT);

  lastPacket = nil_time;

  instr_out = pio_encode_out(pio_null, 16);
  instr_pull = pio_encode_pull(pio_null, 32);
  instr_set = pio_encode_set(pio_pins, 0b11);

  for (int i = 0; i< CHAN_MAX; i++) {
    pio_program_init(i);
    setupDMA(i);
  }

  LOG_SATA("Ready\n");
  while (1){

    if (use_cdrom) {
      gpio_put(CDRST_SNIFF, gpio_get(CDRST));
      gpio_put(CDEN_SNIFF, gpio_get(CDEN));
    } else {
      if (!gpio_get(CDRST)) {
        LOG_SATA("Got a reset Status!\n");
        reset_occured = true;
        wait_out_of_reset();
        status |= CHECK_ERROR;
        errorCode = POWER_OR_RESET_OCCURED;
        restartReadPio();
      }
      if (gpio_get(CDEN)) {
        LOG_SATA("CD is not enabled\n");
        while(gpio_get(CDEN));
        LOG_SATA("CD is enabled now\n");
      }

      bool ejectCurrent = gpio_get(EJECT);
      if ((ejectCurrent != ejectState)) {
        if (!debounceEject) s = get_absolute_time();
        debounceEject = true;
        if (absolute_time_diff_us(s, get_absolute_time()) > 25000) {
          ejectState = ejectCurrent;
          //Eject button pressed, toggle tray position
          if (!ejectCurrent) {
            close_tray((status & TRAY_IN) == 0);
          }
          debounceEject = false;
        }
      } else {
        if (debounceEject && (absolute_time_diff_us(s, get_absolute_time()) > 25000)) debounceEject = false;
      }

      reset_occured = false;
      if (is3doData()) {
        data_in = get3doData();
        handleCommand(data_in);
      }
      handleMediaInterrupt();
    }
  }
}

void _3DO_init() {
  // use_cdrom = true;
  uint offset = -1;

  gpio_init(CDRST);
  gpio_set_dir(CDRST, false);

  gpio_init(EJECT);
  gpio_set_dir(EJECT, false);

  gpio_init(CDWAIT);
  gpio_put(CDWAIT, 1);  //CDWAIT is always 1
  gpio_set_dir(CDWAIT, (!use_cdrom));

  gpio_init(CDEN);
  gpio_set_dir(CDEN, false);

  gpio_init(CDMDCHG);
  gpio_put(CDMDCHG, 1);
  gpio_set_dir(CDMDCHG, (!use_cdrom));


  gpio_init(CDSTEN);
  gpio_put(CDSTEN, 1);
  gpio_set_dir(CDSTEN, (!use_cdrom));

  gpio_init(CDDTEN);
  gpio_put(CDDTEN, 1);
  gpio_set_dir(CDDTEN, (!use_cdrom));

  gpio_init(LED);
  gpio_set_dir(LED, (!use_cdrom));

  gpio_init(CDHRD);
  gpio_set_dir(CDHRD, false);

  gpio_init(CDHWR);
  gpio_set_dir(CDHWR, false);

  gpio_init(DIR_DATA);
  gpio_put(DIR_DATA, DATA_IN); //Input data
  gpio_set_dir(DIR_DATA,  (!use_cdrom));

  gpio_set_dir_masked(DATA_BUS_MASK, DATA_BUS_MASK);
  gpio_init_mask(DATA_BUS_MASK);

  gpio_init(CDRST_SNIFF);
  gpio_put(CDRST_SNIFF, 0);
  gpio_set_dir(CDRST_SNIFF, (!use_cdrom));

  gpio_init(CDEN_SNIFF);
  gpio_put(CDEN_SNIFF, 1);
  gpio_set_dir(CDEN_SNIFF, (!use_cdrom));

  if (!use_cdrom) {

    sm_read = CHAN_MAX;
    offset = pio_add_program(pio0, &read_program);
    read_program_init(pio0, sm_read, offset);
    instr_jmp_read = pio_encode_jmp(offset);


    for (int i = 0; i<32; i++) {
       gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
    }

    pio_gpio_init(pio0, CDD0);
    pio_gpio_init(pio0, CDD1);
    pio_gpio_init(pio0, CDD2);
    pio_gpio_init(pio0, CDD3);
    pio_gpio_init(pio0, CDD4);
    pio_gpio_init(pio0, CDD5);
    pio_gpio_init(pio0, CDD6);
    pio_gpio_init(pio0, CDD7);
  }


  LOG_SATA("wait for msg now\n");


  int clock = clock_get_hz(clk_sys);
  LOG_SATA("Clock is %d\n", clock);

  multicore_launch_core1(core1_entry);
}
