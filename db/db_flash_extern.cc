#include <stdlib.h>
#include <stdio.h>

#include "db/db_flash.h"

namespace leveldb {


		
int flash_flush(Flash_file *f_file);
int flash_sync(Flash_file *f_file);
int flash_entry_sync(Flash_file *f_file);
int is_flash_inited=0;

uint64_t get_file_size(Flash_file *f_file);
int builder_offset;
//externed in env.h
//repalce fwrite
size_t flash_write(const void * ptr, size_t size, size_t count, Flash_file * f_file ){//here we just think the file open mode is "w+"

	//printf("aaa in flash_write, f_file->offset_bytes=%d,f_file->flush_offset=%d \n",f_file->offset_bytes,f_file->flush_offset);
	int write_bytes;
	write_bytes=size*count;
	//get to know whether this write will cause cross segment and handle it --begin
	//if(f_file->file_name.find("LOG")!=-1){
		//printf("extern write, MANEFIST,buffer=%s\n",f_file->buffer);
		//exit(9);
	//}
	if( f_file->offset_bytes + write_bytes > BLOCK_BYTES){
		if(f_file->file_name.find("MANIFEST")!=-1 ){
			//MANIFEST file can overflow
		}
		if(f_file->file_name.find(".log")!=-1 || f_file->file_name.find("MANIFEST")!=-1 ){
			f_file->offset_bytes=ENTRY_BYTES;
			f_file->flush_offset= ENTRY_BYTES;
			f_file->sync_offset=ENTRY_BYTES;
			memset(f_file->buffer,0,ENTRY_BYTES);
		}
		else{
			printf("flash_write, Attention! this will cross segment,fname=%s\n",f_file->file_name.c_str());
			printf("flash_write, Attention,offset_bytes=%d, write_bytes=%d,BLOCK_BYTES=%d\n ",f_file->offset_bytes,write_bytes,BLOCK_BYTES);
			exit(9);	
		}
	}
	
	
	memcpy(f_file->buffer + f_file->offset_bytes,  ptr, size*count);
	
	f_file->offset_bytes = f_file->offset_bytes+write_bytes;//update the offset_bytes
		
	*(uint64_t*)(f_file->buffer+FILE_NAME_BYTES+FILE_ADDR_BYTES)=f_file->offset_bytes; //update the total content bytes
	
	//if(f_file->file_name.find(".log")!=-1){
		
		//printf("flash_write,fname=%s,offset_bytes-ENTRY_BYTES=%d, f_file->seg_offset=%d\n",f_file->file_name.c_str(),f_file->offset_bytes-ENTRY_BYTES, f_file->seg_offset);
		//if(f_file->seg_offset!=4){
		//	printf("not 4, f_file->seg_offset=%d\n",f_file->seg_offset);
			//exit(9);
		//}
		
	//}
	
	//printf("xxx  f_file->offset_bytes=%d,f_file->flush_offset=%d \n",f_file->offset_bytes,f_file->flush_offset);
	
	//*(uint64_t*)(f_file->entry_addr+FILE_NAME_BYTES+FILE_ADDR_BYTES)=f_file->offset_bytes;
	
	
	//if(f_file->offset_bytes - f_file->flush_offset >= 409600 ){ //FLUSH_BYTES
		//flash_flush(f_file);
	
	//}
	//*(f_file->entry_addr+FILE_NAME_BYTES+FILE_ADDR_BYTES+ 8) ='F'; //for test
	
	
	return write_bytes;
	
}
//flash_read replace fread	
size_t flash_read(void * ptr, size_t size, size_t count, Flash_file * f_file ){
	int read_bytes;
	//printf("____hello, I am db_flash_extern, flash read, f_file->offset_bytes=%llu, fname=%s, f_file=%p ,size=%d, count=%d\n", f_file->offset_bytes,f_file->file_name.c_str(), f_file,size,count);
	
	uint64_t content_off=*(uint64_t*)(f_file->entry_addr+FILE_NAME_BYTES+FILE_ADDR_BYTES);
	//printf("____hello, I am db_flash_extern, flash read,f_file->content_bytes=%llu, f_file->addr=%p\n",content_off, f_file->addr);
	if( content_off<= f_file->offset_bytes){// no content to read
		printf("db_flashextern,flashread, no content to read,content_off=%d\n",content_off);
		return 0;
	}
	if(content_off - f_file->offset_bytes < size*count){ //the remain content is less than requested
		read_bytes=content_off - f_file->offset_bytes;
	}
	else{
		read_bytes=size*count;
	}
		//printf("____hello, I am db_flash_extern, before cpy, f_file->offset_bytes=%d,read_bytes=%d\n",f_file->offset_bytes,read_bytes);	
		memcpy(  ptr, f_file->addr+f_file->offset_bytes,read_bytes);
		//printf("____hello, I am db_flash_extern, flash read, ptr=%s\n",ptr);		
		f_file->offset_bytes=f_file->offset_bytes+read_bytes;
	//printf("____hello, I am flash read, addr=%p, \n", f_file->addr);	
		return read_bytes;
}		
	
char * flash_fopen(const char * filename, const char * mode){
	
	//printf("____hello, I am flash_fopen, filename=%s \n",filename);
	
}
int f_fclose(Flash_file *f_file){
	//printf(" Ia m f_fclose \n");
	//f_file->addr==NULL;
	//printf("db_flash_extern.cc, f_close, f_file=%s\n",f_file->file_name.c_str());
	
	//close(f_file->dev_fd);
	
	delete f_file;
	//f_file=NULL;
	
}

int flash_flush(Flash_file *f_file){
	
	 
	 
	uint64_t entry_offset=f_file->seg_offset *(SEGMENT_BYTES);
	
	
	uint64_t l_algined,r_aligned;
	
	l_algined=f_file->flush_offset;
	r_aligned= f_file->offset_bytes;
	 
	l_algined=l_algined/4096;
	l_algined=l_algined*4096;
	
	r_aligned=r_aligned/4096;
	r_aligned=(r_aligned+1)*4096;
	
	//printf("flash_flush,l_algined=%d,r_aligned=%d,entry_offset+l_algined=%d \n",l_algined,r_aligned,entry_offset+l_algined);
	lseek(f_file->dev_fd, entry_offset+l_algined,  SEEK_SET); 
	write(f_file->dev_fd, f_file->buffer+ l_algined, r_aligned- l_algined );//r_aligned- l_algined
	f_file->flush_offset =  f_file->offset_bytes;
	
	return 0;
	/*
	uint64_t cur_flush_offset=entry_offset+ flush_offset;
	uint64_t aligned_offset=offset_bytes/4096;
	//aligned_offset=(aligned_offset-1)*4096;//really align
	aligned_offset=aligned_offset*4096;//really align
	if(aligned_offset<=flush_offset){//do not write
		//or here we can align forward and write the very page, but do not increase flush_offset
		//printf("flash_flush, hit empty\n");
		return 1;
	}
	//printf("block_aligned");
	lseek(f_file->dev_fd, cur_flush_offset,  SEEK_SET); 
	write(f_file->dev_fd, f_file->buffer+ flush_offset,  aligned_offset-  flush_offset);
	f_file->flush_offset =  aligned_offset;
	
	return 0;
	*/
}

int flash_flush_log(Flash_file *f_file){//should forward aligned?

	uint64_t entry_offset=f_file->seg_offset *(SEGMENT_BYTES);
	//printf("flash_flush,flush_offset=%d,offset_bytes=%d,length=%d\n",f_file->flush_offset, f_file->offset_bytes,f_file->offset_bytes-f_file->flush_offset);
	//printf("r_aligned- l_algined, fd=%d\n",f_file->dev_fd);
	uint64_t l_algined,r_aligned;
	l_algined=f_file->flush_offset;
	r_aligned= f_file->offset_bytes;
	
	int algin_unit=512;
	l_algined=l_algined/algin_unit;
	l_algined=l_algined*algin_unit;
	
	r_aligned=r_aligned/algin_unit;
	r_aligned=(r_aligned+1)*algin_unit;
	
	//printf("flash_flush,l_algined=%d,r_aligned=%d,write length=%d \n",l_algined,r_aligned,r_aligned- l_algined);
	lseek(f_file->dev_fd, entry_offset+l_algined,  SEEK_SET); 
	write(f_file->dev_fd, f_file->buffer+ l_algined, r_aligned- l_algined );//r_aligned- l_algined
	f_file->flush_offset =  f_file->offset_bytes;
	
	
	//exit(9);
	return 0;
	
	
	
}
int flash_flush_tail(Flash_file *f_file){

	uint64_t entry_offset=f_file->seg_offset *(SEGMENT_BYTES);
	
	
	uint64_t l_algined,r_aligned;
	
	l_algined=f_file->flush_offset;
	r_aligned= f_file->offset_bytes;
	 
	l_algined=l_algined/4096;
	l_algined=l_algined*4096;
	
	r_aligned=r_aligned/4096;
	r_aligned=(r_aligned+1)*4096;
	
	//printf("flash_flush,l_algined=%d,r_aligned=%d,entry_offset+l_algined=%d \n",l_algined,r_aligned,entry_offset+l_algined);
	lseek(f_file->dev_fd, entry_offset+l_algined,  SEEK_SET); 
	write(f_file->dev_fd, f_file->buffer+ l_algined, r_aligned- l_algined );//r_aligned- l_algined
	f_file->flush_offset =  f_file->offset_bytes;
	
	return 0;
	/*
	 uint64_t flush_offset=f_file->flush_offset;
	 uint64_t offset_bytes= f_file->offset_bytes;
	 //printf("flush_offset=%d,offset_bytes=%d\n",flush_offset,offset_bytes);
	if(offset_bytes<=flush_offset){//no tail
		//printf("flash_flush_tail, hit empty\n");
		return 1;
	}
	uint64_t entry_offset=f_file->seg_offset *(SEGMENT_BYTES);
	uint64_t cur_flush_offset=entry_offset+ flush_offset;

	lseek(f_file->dev_fd, cur_flush_offset,  SEEK_SET);
	
	uint64_t block_aligned=(offset_bytes - flush_offset)/4096;
	block_aligned=(block_aligned+1)*4096;//we always align forward
	
	write(f_file->dev_fd, f_file->buffer+ flush_offset, block_aligned);
	f_file->flush_offset =  f_file->offset_bytes;
	
	return 0;
	*/
}
extern int flash_entry_flush(Flash_file *f_file){

	//if(f_file->file_name.find("MANIFEST")!=-1){
		//printf("extern flash_entry_flush, MANEFIST\n");
		//exit(9);
	//}
	uint64_t entry_offset=f_file->seg_offset *(SEGMENT_BYTES);
	
	lseek(f_file->dev_fd, entry_offset,  SEEK_SET);
	write(f_file->dev_fd, f_file->buffer, ENTRY_BYTES);//write entry
	
	return 0;

}


int flash_sync_log(Flash_file *f_file){	
	int res=-2;
	
	sync_file_range(f_file->dev_fd, f_file->seg_offset *(SEGMENT_BYTES),ENTRY_BYTES,SYNC_FILE_RANGE_WRITE|SYNC_FILE_RANGE_WAIT_AFTER);//entry SYNC_FILE_RANGE_WRITE
	
	
	
	uint64_t sync_point=f_file->seg_offset *(SEGMENT_BYTES) +f_file->sync_offset;
	uint64_t sync_bytes=f_file->flush_offset-f_file->sync_offset;
	res=sync_file_range(f_file->dev_fd, sync_point, sync_bytes , SYNC_FILE_RANGE_WRITE|SYNC_FILE_RANGE_WAIT_AFTER );//SYNC_FILE_RANGE_WRITE
	//res=0;
	f_file->sync_offset = f_file->flush_offset;
	//res=msync(f_file->addr+ f_file->flush_offset, f_file->offset_bytes - f_file->flush_offset ,MS_SYNC);
	if(res!=0){	
		//printf("in db flash_extern fsync res=%d, errno=%d, exit\n",res,errno);
		printf("fname=%s,ffd=%d\n",f_file->file_name.c_str(),f_file->dev_fd);
		exit(3);
	}
	
	//f_file->flush_offset=f_file->offset_bytes - f_file->offset_bytes%MMAP_PAGE;
	
	//printf("msync end,\n");
	if(res==0){
		return 0;
	}
	else{
		return -1;
	}
	
}
int flash_sync(Flash_file *f_file){	
	//return 0;
	int res=-2;
	if(f_file->file_name.find(".log")!=-1){
		//printf("extern sync, MANEFIST\n");
		flash_sync_log(f_file);
		return 0;
	}

	uint64_t entry_offset=f_file->seg_offset *(SEGMENT_BYTES);
	uint64_t l_algined,r_aligned;
	
	l_algined=f_file->sync_offset;
	r_aligned=f_file->flush_offset;
	
	l_algined=l_algined/4096;
	l_algined=l_algined*4096;
	
	r_aligned=r_aligned/4096;
	r_aligned=(r_aligned+1)*4096;
	
	sync_file_range(f_file->dev_fd, entry_offset,ENTRY_BYTES, SYNC_FILE_RANGE_WRITE);

	res=sync_file_range(f_file->dev_fd, entry_offset+l_algined, r_aligned-l_algined, SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE);//|SYNC_FILE_RANGE_WAIT_AFTER 	
	f_file->sync_offset=f_file->flush_offset;
	
	return 0;
	/*
	sync_file_range(f_file->dev_fd, f_file->seg_offset *(SEGMENT_BYTES),ENTRY_BYTES,SYNC_FILE_RANGE_WRITE);//entry SYNC_FILE_RANGE_WRITE
	
	uint64_t sync_point=f_file->seg_offset *(SEGMENT_BYTES) +f_file->sync_offset;
	uint64_t sync_bytes=f_file->flush_offset-f_file->sync_offset;
	res=sync_file_range(f_file->dev_fd, sync_point, sync_bytes , SYNC_FILE_RANGE_WRITE );//SYNC_FILE_RANGE_WRITE
	//res=0;
	f_file->sync_offset = f_file->flush_offset;
	//res=msync(f_file->addr+ f_file->flush_offset, f_file->offset_bytes - f_file->flush_offset ,MS_SYNC);
	if(res!=0){
	
		printf("in db flash_extern fsync res=%d, errno=%d, exit\n",res,errno);
		printf("fname=%s,ffd=%d\n",f_file->file_name.c_str(),f_file->dev_fd);
		exit(3);
	}
	
	//f_file->flush_offset=f_file->offset_bytes - f_file->offset_bytes%MMAP_PAGE;
	
	//printf("msync end,\n");
	if(res==0){
		return 0;
	}
	else{
		return -1;
	}
	*/
}

uint64_t get_file_size(Flash_file *f_file){
	
	uint64_t entry_offset = f_file->seg_offset;
	//printf("in get_file_size, entry_offset=%d\n",entry_offset);
	char r_buffer[ENTRY_BYTES];
	
	memset(r_buffer,0,ENTRY_BYTES);
	uint64_t seek_off=lseek(f_file->dev_fd, entry_offset *(SEGMENT_BYTES)  ,  SEEK_SET);
	
	int res= read(f_file->dev_fd, r_buffer,ENTRY_BYTES);
	
	if( *(r_buffer+8)!=0x64 ){
		int i;
		
		printf("\n");
			printf("in db_flash_extern,r_buffer=%s,entry_offset=%d,seek_off=%lu\n",r_buffer,entry_offset,seek_off);
			printf("in db_flash_extern,getfilesize,haha\n");
			printf("in db_flash_extern,getfilesize,f_file->fname=%s\n",f_file->file_name.c_str());
			
		char r_buffer2[ENTRY_BYTES];
		uint64_t seek_off=lseek(f_file->dev_fd, entry_offset *(SEGMENT_BYTES)  ,  SEEK_SET);	
		int res= read(f_file->dev_fd, r_buffer2,ENTRY_BYTES);	
		
		sleep(1);
			printf("r_buffer:\n");
		for(i=1;i<=48;i++){
			printf("%02x ", *(unsigned char*)(r_buffer+i-1));
			if(i%8==0) printf(" ");
			if(i%16==0) printf("\n");
		}
		printf("r_buffer2:\n");
		for(i=1;i<=48;i++){
			printf("%02x ", *(unsigned char*)(r_buffer2+i-1));
			if(i%8==0) printf(" ");
			if(i%16==0) printf("\n");
		}
		
		
		printf("f_file->buffer\n");
		for(i=1;i<=48;i++){
			printf("%02x ", *(unsigned char*)(f_file->buffer+i-1));
			if(i%8==0) printf(" ");
			if(i%16==0) printf("\n");
		}
		printf("\n");
		
			exit(9);
	}
	/*
	int i;
	for(i=1;i<=48;i++){
		printf("%02x ", *(unsigned char*)(r_buffer+i-1));
		if(i%8==0) printf(" ");
		if(i%16==0) printf("\n");
	}
	printf("\n");
	*/
	uint64_t size=*(uint64_t *)(r_buffer+FILE_NAME_BYTES + FILE_ADDR_BYTES);
	//printf("size=%x\n",size);
	
	return size;
	
}
}// end namespace leveldb 