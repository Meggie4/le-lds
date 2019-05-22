#include <stdint.h>
#include <string>

#include <sys/types.h>
#include <sys/mman.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>    //provides O_RDONLY, 
#include <linux/fs.h>   //provides BLKGETSIZE
#include <sys/ioctl.h>  //provides ioctl()

#include <stdbool.h>

#include <vector>
#include <list>
#include <set>

#define OPEN_ARG

#define PAGE_BYTES 1024  //1KB
#define BLOCK_PAGES 8192 //4096 //4K pages a block
#define BLOCK_BYTES 4194304 //16777216//4194304// 8388608 //4194304 //4MB
#define SEGMENT_BLOCKS 1//#define AREA_BLOCKS 1 //1 block
#define SEGMENT_BYTES (SEGMENT_BLOCKS*BLOCK_BYTES)
#define VERSION_BYTES 4
#define MAGIC "MMAPKV"
#define MAGIC_BYTES 6

#define FILE_NAME_BYTES 24 //file name
#define FILE_ADDR_BYTES 8 //the segment offset
#define FILE_BYTES 8 //content bytes
#define ENTRY_BYTES 4096//(FILE_NAME_BYTES+FILE_ADDR_BYTES+..)
//segment
#define EXTENDED_ENTRY_NUM_BYTE_OFFSET 22
#define NON_LDB_ENTRY_NUM 32
#define DEV_NAME_LENGTH 9 //  /dev/sdp/

#define MMAP_PAGE 4096

#define FLUSH_BYTES 4096
namespace leveldb {

class Flash_file;

uint64_t AllocSeg(uint64_t next_file_number_);
std::string get_file_name(const std::string& db_name);

uint64_t entry_hash(const std::string& dbname);

class Flash{
	
		public:
		#define FLASH_OPEN_ARG  const std::string& dbname
		virtual int flash_open(FLASH_OPEN_ARG);
		virtual int flash_format(const std::string& dbname);
		virtual int flash_init(const std::string& dbname);
		
		
		int set_valid(char* addr, uint64_t offset, unsigned char value);
		
		unsigned char * frequency_info;
		unsigned char * validation_bitmap;
		unsigned char * level0_entries;
		unsigned char * info;
		char * files_name_block;
		char *dev;
		//char *dev_r;
		
		std::string db_dev;
		
		std::set<std::string> f_filenames;
		std::vector<int> f_handles;
		
		std::string db_name;
		int dev_fd;
		//FILE * dev_file;
		
		//df_flash_env
		int rename(const std::string&src, const std::string&target) ;
		int  remove(const std::string& filename);
		virtual int access(const std::string& fname, int flag);
		int create_entry(const std::string& fname,uint64_t entry_offset, uint64_t segment_offset);
		int delete_entry(const std::string& fname, int mode);
		//virtual size_t read(void * ptr, size_t size, size_t count, char * addr );
		#define OPEN_FILE_ARG const std::string& fname, const char * mode
		Flash_file*  open_file(OPEN_FILE_ARG);
		int x();
		uint64_t segment_total;
		uint64_t ldb_entry_total;
		
		//db_flash_tools.cc
		
		uint64_t get_entry_offset_by_name(const std::string& short_name);
		uint64_t get_seg_offset_by_name(const std::string& short_name);
		
		
		uint64_t size;
		Flash(){
			printf("________-I am db_flash.h,  Flash construction\n");
		}
		void get(){
			printf("Hello, I'm set, size=%d\n",this->size);
		}
		
	
};

class Flash_file{
	public:
	char * addr;// the file segment addr pointer
	char *entry_addr;//the entry addr pointer
	uint64_t offset_bytes;//the content  offset. This is the position the current operation should begin.
	
	uint64_t content_bytes;// total content bytes. This is the total bytes the current file has stored.
	
	std::string file_name;//for test
	
	//char *flush_addr;//for msync
	uint64_t flush_offset;//for msync
	
	uint64_t sync_offset;//for 
	
	uint64_t seg_offset;
	int dev_fd;
	void *buffer;
	//char *w_buffer;
	
	Flash_file(){
		offset_bytes= ENTRY_BYTES;
		flush_offset= ENTRY_BYTES;
		sync_offset=ENTRY_BYTES;
		
		//this->buffer=(char *)malloc(SEGMENT_BYTES);	
		posix_memalign(&(this->buffer),512,SEGMENT_BYTES);	
		memset(this->buffer,0,ENTRY_BYTES);
		
	}
	~Flash_file(){
		//printf("I am ~Flash_file\n");
		free(buffer);
		
	}
	
};

}