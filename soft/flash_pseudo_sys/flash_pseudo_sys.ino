/*
   W25Q128FV Serial Flasher
   
   (c) Frank Boesing, 2014,Dec
   GNU License Version 3.
   Teensy Audio Shield (c) Paul Stoffregen 
   W25Q128FV - Library  (c) Pete (El Supremo) 
   Thank you both!!!!
      
   Reads directory "/SERFLASH" on SD and burns 
   all files to the optional serial flash.
  
   Version 20141227
   
   + file info / extraction (mxmxmx):
   
   file_count: stored in bytes 0, 1 / page 0x0;
   end of data: stored in bytes 2,3,4,5 / page 0x0;
   file_info : stored in 12 byte packets: 4 byte [adress] + 8 byte [name], from byte 2 / page 0x0;
   
   ie:
   
   [b0]  [b1] [b02 - b05] [b06 - b9] [b10 - b17] [b18 - b21] [b22 - b29] [etc .. ]
   
   [# files ] [ data end ] [f 0 adr] [f 0 name ] [f 1 adr  ] [f 1 name ] [etc .. ]
      
*/

#include <Audio.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <flash_spi.h>

#define FLASHSIZE 0x1000000
#define PAGE      256
#define DIRECTORY "SERFLASH"
#define MAX_FILES 256 
#define MAX_LEN 0x8                    // file name size
#define INFO_SLOT_SIZE (MAX_LEN + 0x4) // address size + file name
#define _OFFSET 0x6                    // file # in bytes 0,1; end pos bytes 2,3,4,5 
#define INFO_ADR 0x0                   // info adr.

uint16_t NUM_INFO_PAGES; 

File dir;
File entry;
unsigned char id_tab[32];
unsigned pos;
unsigned page;
int fsize = 0;
int fcnt, fcnt_all = 0;
unsigned char buf[PAGE];

String filename[MAX_FILES];
unsigned int file_position[MAX_FILES];
uint8_t _EXT = 0;

/* recovered adresses + files go here: */

uint32_t file_adr[MAX_FILES+0x1]; // + end of data
uint32_t file_len[MAX_FILES];     // length
String   file_name[MAX_FILES];    // names 


bool verify(void)
{
  
    unsigned char buf2[PAGE];

    fcnt = 0;
    pos = 0; 
    page = 0;   
    Serial.println("Verify.");
    // file number?
    flash_read_pages(buf2, INFO_ADR, 1);
    uint16_t num_files = ((uint16_t)(buf2[0]) << 0x8) + buf2[1]; // extract file #
    uint16_t num_pages = 0x1 + ((num_files*INFO_SLOT_SIZE + _OFFSET) >> 0x8); // page offset
   
    if (fcnt_all != num_files)  { 
              Serial.println("Files on flash ? --> file #: no match"); 
              Serial.println(""); 
              return false; 
    }
    else Serial.printf("Files on flash ? --> # %d file(s): ok \r\n", num_files); 
    
    page += num_pages;   // audio data starts here
   
    dir = SD.open(DIRECTORY); 
    while(1) {
      entry = dir.openNextFile();
      if (!entry) break;
      pos = page * PAGE;
      Serial.printf("%d. Verifying \"%s\" at file_position: 0x%07X...", fcnt+1, entry.name(), pos);
      filename[fcnt] = entry.name();
      file_position[fcnt] = pos;
      int rd =0;
      do {
        memset(buf, 0xff, PAGE);
        rd = entry.read(buf, PAGE);
        flash_read_pages(buf2, page, 1);
        int v = 0;
        for (int i=0; i<PAGE; i++) v+= buf[i]-buf2[i];
        if (v) {Serial.println("no match"); return false;}
        pos += rd;         
        page++;
      } while (rd==PAGE);          
      Serial.printf("ok.\r\n");
      entry.close(); 
      fcnt++;
    }
    return true;
}

void flash(void)
{
  
   unsigned char buf[PAGE];
    
   fcnt = 0;
   pos =  0;
   page = NUM_INFO_PAGES; // file info offset 
    
   dir = SD.open(DIRECTORY);
   while(fcnt < fcnt_all) {
      entry = dir.openNextFile();
      if (!entry) break;
      pos = page * PAGE;
      Serial.printf("-- > Flashing file # %d : \"%s\" at file_position: 0x%07X ...\r\n", fcnt+1, entry.name(), pos);
      file_position[fcnt] = pos; // store file_position
      int rd =0;
      do {
          memset(buf, 0xff, PAGE);          
          rd = entry.read(buf, PAGE);
          pos += rd;         
          flash_page_program(buf,page);
          page++;
      } while (rd==PAGE);          
    entry.close(); 
    fcnt++;
    }
    
    // store INFO_PAGES:
    uint32_t end_of_data = page * PAGE;
    uint16_t slot = 0;
    uint8_t pseudo_file_sys[NUM_INFO_PAGES*PAGE]; 
    Serial.println("");
    Serial.printf("Generating file info (%d page(s)):", NUM_INFO_PAGES);
    Serial.println("");
    Serial.printf("data stop %d at position", end_of_data);
    memset(pseudo_file_sys, 0xff, NUM_INFO_PAGES*PAGE); 
    // file # 
    pseudo_file_sys[slot] = fcnt>>8;       // 0
    slot++;
    pseudo_file_sys[slot] = fcnt;          // 1
    slot++;
    pseudo_file_sys[slot] = end_of_data>>24; // 2
    slot++; 
    pseudo_file_sys[slot] = end_of_data>>16; // 3
    slot++;
    pseudo_file_sys[slot] = end_of_data>>8;  // 4
    slot++;
    pseudo_file_sys[slot] = end_of_data;     // 5
    slot++;
    Serial.println("");
    // adress + file-names
    uint8_t len = MAX_LEN; 
    for (int i = 0; i < fcnt; i++) {
       
        uint32_t f_pos = file_position[i];
        // split address:
        pseudo_file_sys[slot] = f_pos>>24;
        slot++;
        pseudo_file_sys[slot] = f_pos>>16;
        slot++;
        pseudo_file_sys[slot] = f_pos>>8;
        slot++; 
        pseudo_file_sys[slot] = f_pos;
        slot++;
        Serial.printf("-- > file # %d (of %d) :: 0x%07X ...\r\n", i, fcnt, f_pos);
        // write name (8.3) -- skip the extension
        _EXT = 0; //
        const char *tmp = filename[i].c_str();
        for (int j = 0; j < len; j++) {
             pseudo_file_sys[slot] = makenice(tmp[j], _EXT);
             slot++;
        }
    }    
    // now flash the info page(s):
    uint8_t pages = NUM_INFO_PAGES; 
    page = INFO_ADR; // start from 0x0
    Serial.println("");   
    while(pages) {
        Serial.printf("-- > Flashing info page # %d (of %d) at file_position: 0x%07X ...\r\n", page+1, NUM_INFO_PAGES, page*PAGE);
        uint8_t *sys = pseudo_file_sys+page*PAGE;
        flash_page_program(sys, page);
        pages--;
        page++;
    }
    Serial.printf("done.\r\n");
    Serial.println("");
}

bool extract(void)
{
    uint16_t num_files;
    uint16_t num_pages;
    uint32_t end_of_data;
    unsigned char buf[PAGE];
  
    Serial.println("");
    Serial.println("Extracting file info:");
    // file number?
    flash_read_pages(buf, INFO_ADR, 1);
    num_files = ((uint16_t)(buf[0]) << 0x8) + buf[1];               // extract file #
    
    if (!num_files || num_files > MAX_FILES)  { 
              Serial.println("-->  no files found"); 
              return false; 
    }
    else Serial.printf("-- > found %d file(s): ok \r\n", num_files); 
    //
    end_of_data = (buf[2] << 24) + (buf[3] << 16) + (buf[4] << 8) + buf[5];
    
    // ... ok, now, read file info:
    num_pages = 0x1 + ((num_files*INFO_SLOT_SIZE + _OFFSET) >> 0x8);  // info-page size
    Serial.printf("-- > audio data starts at 0x%07X, ends at position: 0x%07X ...\r\n", num_pages*PAGE, end_of_data);
    unsigned char tmp_buf[PAGE*num_pages];
    flash_read_pages(tmp_buf, INFO_ADR, num_pages);
    parse_INFO_PAGES(tmp_buf, num_files); 
    
    // calc. length from positions:
    uint16_t _pos = 0;
    file_adr[num_files] = end_of_data; 
    
    while (_pos < num_files) {
   
           file_len[_pos] = file_adr[_pos+1] - file_adr[_pos]; // length in bytes
           _pos++;
          
    }
    // print file info:
    _pos = 0;
    while (_pos < num_files) {
         
         Serial.printf("%s -- > file_adr[%d] = 0x%07X.  (%d bytes) \r\n", file_name[_pos].c_str(), _pos, file_adr[_pos], file_len[_pos]);
         _pos++;
    }
    
    Serial.println("done.");Serial.println("");
    return true;
}
    
void erase(void) {
    Serial.println("Erasing chip....");
    flash_chip_erase(true);
    Serial.println("done.");
}

void parse_INFO_PAGES(uint8_t *fileinfo, uint8_t _files) {
  
     fileinfo += _OFFSET; // skip file info # bytes
     uint8_t _f = _files;
     while (_f) {
         uint32_t tmp, adr;
         tmp = *fileinfo; fileinfo++; // 1
         adr  = tmp << 24; 
         tmp = *fileinfo; fileinfo++; // 2
         adr += tmp << 16;
         tmp = *fileinfo; fileinfo++; // 3
         adr += tmp << 8;
         tmp = *fileinfo; fileinfo++; // 4
         adr += tmp;
         
         file_adr[_files-_f] = adr;   // file pos
         
         uint8_t len = MAX_LEN; // names
         char _name[len+0x1];   // tmp String for name
         while (len) {
               _name[MAX_LEN-len] = *fileinfo;  
               len--;   
               fileinfo++;  
         };  
         _name[MAX_LEN] = '\0';
         file_name[_files-_f] = _name;
         //Serial.printf(" -- > file_adr[%d] = 0x%07X ......\r\n", _files - _f, adr);
        _f--;
     }
     
  Serial.println("");
};

char makenice(char _n, uint8_t _ext) {
   
   if (_ext) return ' ';
   
   char tmp = _n;   
   if (tmp == '.') _EXT = 1; 
   if (tmp >= '0' && tmp <= '9') tmp = tmp;
   else if (tmp >= 'A' && tmp  <= 'Z' ) tmp = tmp + 'a' - 'A';
   else tmp = ' ';
   return tmp;
}


void setup()
{
  SPI.setMOSI(7);
  SPI.setSCK(14);
  // Open serial communications and wait for port to open:
  Serial.begin(9600);
  while (!Serial) {;}  
  delay(1000); 
  Serial.print("\r\n\r\nW25Q128FV Serial Flasher \r\nInitializing SD card...");
  if (!SD.begin(10)) {
    Serial.println("failed!");
    return;
  }  
  Serial.println("done.\r\n");
  dir = SD.open(DIRECTORY);
  fsize = 0;
  fcnt = 0;
  while(1) 
  {
    entry = dir.openNextFile();
    if (!entry || fcnt >= MAX_FILES) break;    
    int s = entry.size();   
    if ( (s & 0xff) > 0) s |= 0xff;
    filename[fcnt] = entry.name(); // --> filenames
    Serial.printf("%s\t\t\t%d bytes\r\n", filename[fcnt].c_str(), s);
    fsize += s;   
    entry.close();  
    fcnt++;
  } 
  dir.close();
  fcnt_all = fcnt;
  
  Serial.printf("\t\t\t%d file(s): %d bytes \r\n", fcnt, fsize);
  NUM_INFO_PAGES = 0x1 + ((fcnt*INFO_SLOT_SIZE + _OFFSET) >> 0x8); // == # pages; 12 bytes: pos + name + fcnt;
  
  Serial.printf("\t\t\tPages consumed for file info: %d page(s)\r\n", NUM_INFO_PAGES);
  fsize += NUM_INFO_PAGES*PAGE;
  Serial.printf("\t\t\t--> total: %d bytes \r\n", fsize);
  Serial.println("");
  
  if (!fsize) goto ready;
  if (fsize < FLASHSIZE) {
      flash_init(15);
      //flash_init();
      int flashstatus = flash_read_status();
      flash_read_id(id_tab);
      Serial.printf("Flash Status: 0x%X, ID:0x%X,0x%X,0x%X,0x%X ", flashstatus , id_tab[0], id_tab[1], id_tab[2], id_tab[3]);   
      if (id_tab[0]!=0xef || id_tab[1]!=0x40 || id_tab[2]!=0x18 || id_tab[3]!=0x00) {Serial.println(" is not ok."); goto end;}
      Serial.printf(" is ok.\r\nFile(s) fit(s) in serial flash, %d Bytes remaining.\r\n\r\n", FLASHSIZE - fsize);

      Serial.print("Check flash content: \r\n");
      extract();    
      if (verify()) { Serial.println("Flash content ok. Nothing to do."); goto end; }
      erase();
      flash();      
      verify();     
      extract();
      
end:            
      dir.close();
  }
  else Serial.println("Does not fit.");  
  
ready:  
  Serial.println("Ready.");
}



void loop()
{}


