#include "stdio.h"
#include "string.h"
#include "hardware.h"

#include "menu.h"
#include "archie.h"
#include "hdd.h"
#include "user_io.h"
#include "data_io.h"
#include "debug.h"

#define MAX_FLOPPY  2

#define CONFIG_FILENAME  "ARCHIE  CFG"

typedef struct {
  unsigned long system_ctrl;     // system control word
  char rom_img[12];              // rom image file name
  char cmos_img[12];             // cmos image file name
  hardfileTYPE  hardfile[2];
} archie_config_t;

static archie_config_t config;

fileTYPE floppy[MAX_FLOPPY];

#define archie_debugf(a, ...) iprintf("\033[1;31mARCHIE: " a "\033[0m\n", ##__VA_ARGS__)
// #define archie_debugf(a, ...)
#define archie_x_debugf(a, ...) iprintf("\033[1;32mARCHIE: " a "\033[0m\n", ##__VA_ARGS__)

enum state { STATE_HRST, STATE_RAK1, STATE_RAK2, STATE_IDLE, 
	     STATE_WAIT4ACK1, STATE_WAIT4ACK2, STATE_HOLD_OFF } kbd_state;

// archie keyboard controller commands
#define HRST    0xff
#define RAK1    0xfe
#define RAK2    0xfd
#define RQPD    0x40         // mask 0xf0
#define PDAT    0xe0         // mask 0xf0
#define RQID    0x20
#define KBID    0x80         // mask 0xc0
#define KDDA    0xc0         // new key down data, mask 0xf0
#define KUDA    0xd0         // new key up data, mask 0xf0
#define RQMP    0x22         // request mouse data
#define MDAT    0x00         // mouse data, mask 0x80
#define BACK    0x3f
#define NACK    0x30         // disable kbd scan, disable mouse
#define SACK    0x31         // enable kbd scan, disable mouse
#define MACK    0x32         // disable kbd scan, enable mouse
#define SMAK    0x33         // enable kbd scan, enable mouse
#define LEDS    0x00         // mask 0xf8
#define PRST    0x21         // nop

#define QUEUE_LEN 8
static unsigned char tx_queue[QUEUE_LEN][2];
static unsigned char tx_queue_rptr, tx_queue_wptr;
#define QUEUE_NEXT(a)  ((a+1)&(QUEUE_LEN-1))

static unsigned long ack_timeout;
static short mouse_x, mouse_y; 

#define FLAG_SCAN_ENABLED  0x01
#define FLAG_MOUSE_ENABLED 0x02
static unsigned char flags;

// #define HOLD_OFF_TIME 2
#ifdef HOLD_OFF_TIME
static unsigned long hold_off_timer;
#endif

static void nice_name(char *dest, char *src) {
  char *c;

  // copy and append nul
  strncpy(dest, src, 8);
  for(c=dest+7;*c==' ';c--); c++;
  *c++ = '.';
  strncpy(c, src+8, 3);
  for(c+=2;*c==' ';c--); c++;
  *c++='\0';
}

static char buffer[17];  // local buffer to assemble file name (8+.+3+\0)

char *archie_get_rom_name(void) {
  nice_name(buffer, config.rom_img);
  return buffer;
}

char *archie_get_cmos_name(void) {
  nice_name(buffer, config.cmos_img);
  return buffer;
}

char *archie_get_floppy_name(char i) {
  if(!floppy[i].size) 
    strcpy(buffer, "* no disk *");
  else
    nice_name(buffer, floppy[i].name);

  return buffer;
}

void archie_save_config(void) {
  fileTYPE file;

  // save configuration data
  if (FileOpen(&file, CONFIG_FILENAME))  {
    archie_debugf("Existing conf file size: %lu", file.size);
    if(file.size != sizeof(archie_config_t)) {
      file.size = sizeof(archie_config_t);
      if (!UpdateEntry(&file))
	return;
    }
  } else {
    archie_debugf("Creating new config");
    strncpy(file.name, CONFIG_FILENAME, 11);
    file.attributes = 0;
    file.size = sizeof(archie_config_t);
    if(!FileCreate(0, &file)) {
      archie_debugf("File creation failed.");
      return;
    }
  }

  // finally write the config
  memcpy(sector_buffer, &config, sizeof(archie_config_t));
  FileWrite(&file, sector_buffer);
}

void archie_set_floppy(char i, fileTYPE *file) {
  if(!file) {
    archie_debugf("Floppy %d eject", i);
    floppy[i].size = 0;
  } else {
    archie_debugf("Floppy %d insert %.11s", i, file->name);
    floppy[i] = *file;
  }
}

char archie_floppy_is_inserted(char i) {
  return(floppy[i].size != 0);
}

void archie_save_cmos() {
  fileTYPE file;

  archie_debugf("Saving CMOS file");
  if (FileOpen(&file, config.cmos_img)) {
    archie_debugf("Existing CMOS file size: %lu", file.size);
  } else {
    archie_debugf("Creating new CMOS file");
    strncpy(file.name, config.cmos_img, 11);
    file.attributes = 0;
    file.size = 256;
    if(!FileCreate(0, &file)) {
      archie_debugf("File creation failed.");
      return;
    }
  }

  data_io_file_rx(&file, 0x03, 256);

}

void archie_set_cmos(fileTYPE *file) {
  if(!file) return;

  archie_debugf("CMOS file %.11s with %lu bytes to send", 
		file->name, file->size);

  // save file name
  memcpy(config.cmos_img, file->name, 11);

  data_io_file_tx(file, 0x03);
}

void archie_set_rom(fileTYPE *file) {
  if(!file) return;

  archie_debugf("ROM file %.11s with %lu bytes to send", 
		file->name, file->size);

  // save file name
  memcpy(config.rom_img, file->name, 11);

  data_io_file_tx(file, 0x01);
}

static void archie_kbd_enqueue(unsigned char state, unsigned char byte) {
  if(QUEUE_NEXT(tx_queue_wptr) == tx_queue_rptr) {
    archie_debugf("KBD tx queue overflow");
    return;
  }

  archie_debugf("KBD ENQUEUE %x (%x)", byte, state);
  tx_queue[tx_queue_wptr][0] = state;
  tx_queue[tx_queue_wptr][1] = byte;
  tx_queue_wptr = QUEUE_NEXT(tx_queue_wptr);
} 

static void archie_kbd_tx(unsigned char state, unsigned char byte) {
  archie_debugf("KBD TX %x (%x)", byte, state);
  spi_uio_cmd_cont(0x05);
  spi8(byte);
  DisableIO();

  kbd_state = state;
  ack_timeout = GetTimer(10);  // 10ms timeout
}

static void archie_kbd_send(unsigned char state, unsigned char byte) {
  // don't send if we are waiting for an ack
  if((kbd_state != STATE_WAIT4ACK1)&&(kbd_state != STATE_WAIT4ACK2)) 
    archie_kbd_tx(state, byte);
  else
    archie_kbd_enqueue(state, byte);
}

static void archie_kbd_reset(void) {
  archie_debugf("KBD reset");
  tx_queue_rptr = tx_queue_wptr = 0;
  kbd_state = STATE_HRST;
  mouse_x = mouse_y = 0;
  flags = 0;
}

void archie_init(void) {
  fileTYPE file;
  char i;

  archie_debugf("init");

  // set config defaults
  config.system_ctrl = 0;
  strcpy(config.rom_img, "RISCOS  ROM");
  strcpy(config.cmos_img, "CMOS    RAM");

  config.hardfile[0].enabled = HDF_FILE;
  strncpy(config.hardfile[0].name, "ARCHIE1 ", sizeof(config.hardfile[0].name));
  config.hardfile[0].long_name[0]=0;
  config.hardfile[1].enabled = HDF_FILE;
  strncpy(config.hardfile[1].name, "ARCHIE2 ", sizeof(config.hardfile[1].name));
  config.hardfile[1].long_name[0]=0;

  // try to load config from card
  if(FileOpen(&file, CONFIG_FILENAME)) {
    if(file.size == sizeof(archie_config_t)) {
      FileRead(&file, sector_buffer);
      memcpy(&config, sector_buffer, sizeof(archie_config_t));
    } else
      archie_debugf("Unexpected config size %d != %d", 
		    file.size, sizeof(archie_config_t));
  } else
    archie_debugf("No %.11s config found", CONFIG_FILENAME);

  // upload rom file
  if(FileOpen(&file, config.rom_img))
    archie_set_rom(&file);
  else 
    archie_debugf("ROM %.11s no found", config.rom_img);

  // upload ext file
  if(FileOpen(&file, "RISCOS  EXT")) {
    archie_debugf("Found RISCOS.EXT, uploading it");
    data_io_file_tx(&file, 0x02);
  } else 
    archie_debugf("RISCOS.EXT no found");

  // upload cmos file
  if(FileOpen(&file, config.cmos_img))
    archie_set_cmos(&file);
  else 
    archie_debugf("CMOS %.11s no found", config.cmos_img);

  // try to open default floppies
  for(i=0;i<MAX_FLOPPY;i++) {
    char fdc_name[] = "FLOPPY0 ADF";
    fdc_name[6] = '0'+i;
    if (FileOpen(&floppy[i], fdc_name)) {
      user_io_file_mount(&floppy[i], i);
      archie_debugf("Inserted floppy %d with %d bytes", i, floppy[i].size);
    } else
      floppy[i].size = 0;
  }

  // open hdd image(s)
  hardfile[0] = &config.hardfile[0];
  hardfile[1] = &config.hardfile[1];

  OpenHardfile(0);
  OpenHardfile(1);

  archie_kbd_send(STATE_RAK1, HRST);
  ack_timeout = GetTimer(20);  // give archie 20ms to reply
}

void archie_kbd(unsigned short code) {
  archie_debugf("KBD key code %x", code);

  // don't send anything yet if we are still in reset state
  if(kbd_state <= STATE_RAK2) {
    archie_debugf("KBD still in reset");
    return;
  }

  // ignore any key event if key scanning is disabled
  if(!(flags & FLAG_SCAN_ENABLED)) {
    archie_debugf("KBD keyboard scan is disabled!");
    return;
  }

  // select prefix for up or down event
  unsigned char prefix = (code&0x8000)?KUDA:KDDA;

  archie_kbd_send(STATE_WAIT4ACK1, prefix | (code>>4)); 
  archie_kbd_send(STATE_WAIT4ACK2, prefix | (code&0x0f));
}

void archie_mouse(unsigned char b, char x, char y) {
  archie_debugf("KBD MOUSE X:%d Y:%d B:%d", x, y, b);

  // max values -64 .. 63
  mouse_x += x;
  if(mouse_x >  63) mouse_x =  63;
  if(mouse_x < -64) mouse_x = -64;

  mouse_y -= y;
  if(mouse_y >  63) mouse_y =  63;
  if(mouse_y < -64) mouse_y = -64;

  // don't send anything yet if we are still in reset state
  if(kbd_state <= STATE_RAK2) {
    archie_debugf("KBD still in reset");
    return;
  }

  // ignore any mouse movement if mouse is disabled or if nothing to report
  if((flags & FLAG_MOUSE_ENABLED) && (mouse_x || mouse_y)) {
    // send asap if no pending byte
    if(kbd_state == STATE_IDLE) {
      archie_kbd_send(STATE_WAIT4ACK1, mouse_x & 0x7f); 
      archie_kbd_send(STATE_WAIT4ACK2, mouse_y & 0x7f);
      mouse_x = mouse_y = 0;
    }
  }

  // ignore mouse buttons if key scanning is disabled
  if(flags & FLAG_SCAN_ENABLED) {
    static const uint8_t remap[] = { 0, 2, 1 };
    static unsigned char buts = 0;
    uint8_t s;

    // map all three buttons
    for(s=0;s<3;s++) {
      uint8_t mask = (1<<s);
      if((b&mask) != (buts&mask)) {
	unsigned char prefix = (b&mask)?KDDA:KUDA;
	archie_kbd_send(STATE_WAIT4ACK1, prefix | 0x07); 
	archie_kbd_send(STATE_WAIT4ACK2, prefix | remap[s]);
      }
    }
    buts = b;
  }
}

static void archie_check_queue(void) {
  if(tx_queue_rptr == tx_queue_wptr)
    return;

  archie_kbd_tx(tx_queue[tx_queue_rptr][0], tx_queue[tx_queue_rptr][1]); 
  tx_queue_rptr = QUEUE_NEXT(tx_queue_rptr);
}

void archie_handle_kbd(void) {

#ifdef HOLD_OFF_TIME
  if((kbd_state == STATE_HOLD_OFF) && CheckTimer(hold_off_timer)) {
    archie_debugf("KBD resume after hold off");
    kbd_state = STATE_IDLE;
    archie_check_queue();
  }
#endif

  // timeout waiting for ack?
  if((kbd_state == STATE_WAIT4ACK1) || (kbd_state == STATE_WAIT4ACK2)) {
    if(CheckTimer(ack_timeout)) {
      if(kbd_state == STATE_WAIT4ACK1)
	archie_debugf(">>>> KBD ACK TIMEOUT 1ST BYTE <<<<");
      if(kbd_state == STATE_WAIT4ACK2)
	archie_debugf(">>>> KBD ACK TIMEOUT 2ND BYTE <<<<");

      kbd_state = STATE_IDLE;
    }
  }

  // timeout in reset sequence?
  if(kbd_state <= STATE_RAK2) {
    if(CheckTimer(ack_timeout)) {
      archie_debugf("KBD timeout in reset state");
      
      archie_kbd_send(STATE_RAK1, HRST);
      ack_timeout = GetTimer(20);  // 20ms timeout
    }
  }

  spi_uio_cmd_cont(0x04);
  if(spi_in() == 0xa1) {
    unsigned char data = spi_in();
    DisableIO();
    
    archie_debugf("KBD RX %x", data);

    switch(data) {
      // arm requests reset
    case HRST:
      archie_kbd_reset();
      archie_kbd_send(STATE_RAK1, HRST);
      ack_timeout = GetTimer(20);  // 20ms timeout
      break;

      // arm sends reset ack 1
    case RAK1:
      if(kbd_state == STATE_RAK1) {
	archie_kbd_send(STATE_RAK2, RAK1);
	ack_timeout = GetTimer(20);  // 20ms timeout
      } else 
	kbd_state = STATE_HRST;
      break;

      // arm sends reset ack 2
    case RAK2:
      if(kbd_state == STATE_RAK2) { 
	archie_kbd_send(STATE_IDLE, RAK2);
	ack_timeout = GetTimer(20);  // 20ms timeout
      } else 
	kbd_state = STATE_HRST;
      break;

      // arm request keyboard id
    case RQID:
      archie_kbd_send(STATE_IDLE, KBID | 1);
      break;

      // arm acks first byte
    case BACK:
      if(kbd_state != STATE_WAIT4ACK1) {
          archie_debugf("KBD unexpected BACK, resetting KBD");
          kbd_state = STATE_HRST;
      } else {
#ifdef HOLD_OFF_TIME
          // wait some time before sending next byte
          archie_debugf("KBD starting hold off");
          kbd_state = STATE_HOLD_OFF;
          hold_off_timer = GetTimer(10);
#else
          kbd_state = STATE_IDLE;
          archie_check_queue();
#endif
      }
      break;

      // arm acks second byte
    case NACK:
    case SACK:
    case MACK:
    case SMAK:

      if(((data == SACK) || (data == SMAK)) && !(flags & FLAG_SCAN_ENABLED)) {
	archie_debugf("KBD Enabling key scanning");
	flags |= FLAG_SCAN_ENABLED;
      }

      if(((data == NACK) || (data == MACK)) && (flags & FLAG_SCAN_ENABLED)) {
	archie_debugf("KBD Disabling key scanning");
	flags &= ~FLAG_SCAN_ENABLED;
      }

      if(((data == MACK) || (data == SMAK)) && !(flags & FLAG_MOUSE_ENABLED)) {
	archie_debugf("KBD Enabling mouse");
	flags |= FLAG_MOUSE_ENABLED;
      }

      if(((data == NACK) || (data == SACK)) && (flags & FLAG_MOUSE_ENABLED)) {
	archie_debugf("KBD Disabling mouse");
	flags &= ~FLAG_MOUSE_ENABLED;
      }
      
      // wait another 10ms before sending next byte
#ifdef HOLD_OFF_TIME
      archie_debugf("KBD starting hold off");
      kbd_state = STATE_HOLD_OFF;
      hold_off_timer = GetTimer(10);
#else
      kbd_state = STATE_IDLE;
      archie_check_queue();
#endif
      break;
    }
  } else
    DisableIO();
}

void archie_handle_hdd(void) {
  unsigned char  c1;

  EnableFpga();
  c1 = SPI(0); // cmd request
  SPI(0);
  SPI(0);
  SPI(0);
  SPI(0);
  SPI(0);
  DisableFpga();

  HandleHDD(c1, 0);
}

void archie_poll(void) {
  archie_handle_kbd();
  archie_handle_hdd();
}
