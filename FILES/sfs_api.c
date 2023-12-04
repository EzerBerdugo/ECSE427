#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <fuse.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <strings.h>  
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "disk_emu.h"
#include "sfs_api.h"

#define BLOCK_SIZE 1024 //CCdisk is divided into sectors of fixed size of 1024 bytes
#define NUMBER_OF_BLOCKS 1024
#define BUF_SIZE 4096 // use a buffer size of 4096 bytes
#define OUTPUT_MODE 0700 //protection bits for output 
#define MAGIC 0xABCD0005
#define NUMBER_OF_INODES 100
#define SYSTEM_NAME "ECSE-427-ASSIGNMENT"
#define MAXIMUM_FILE_SIZE 30000
#define MAXIMUM_FILE_NAME 20
//BLOCK_SIZE * ((BLOCK_SIZE / sizeof(int)) + 12) 
#define MAXIMUM_OPEN_FILES 100

//Define Data Structres
typedef struct {
    int magic;
    int block_size;
    int file_system_size;
    int inode_table_length;
    int root_directory;
} SuperBlock;

typedef struct {
    unsigned int mode;
    unsigned int link_count;
    unsigned int uid;
    unsigned int gid;
    unsigned int size;
    unsigned int pointers[12];
    unsigned int indirect_pointers; 
} iNode;

typedef struct { //Two seperate pointers for FDT was causing too many initialization issues, went with 1 pointer instead and dynamically switch between them
    int inode_number;
    iNode* inode;
    int rwpointer;
} FileDescriptorTable;

typedef struct {
    int i_node; 
    char file_name[MAXIMUM_FILE_NAME+1];
}DirectoryEntry;

//Using a array based approach for implementing a bitmap to keep track of available blocks
int bit_map[128] = { [0 ... 127] = UINT8_MAX };

SuperBlock superblock;
iNode iNodeTable[NUMBER_OF_INODES];
DirectoryEntry rootDirectory[NUMBER_OF_INODES];
FileDescriptorTable fdt[NUMBER_OF_INODES];
int directory_location;
int iNodeTableStatus[NUMBER_OF_INODES]; 
int indirectPointerBlock[BLOCK_SIZE/sizeof(int)];

//get_directory_inode: Finds the I-Node associated with the given file in the root directory.
//--------------------------------------------------------------------------------------------------------//
int get_directory_inode(char *name)
{
	char *temp = malloc(sizeof(char) * (MAXIMUM_FILE_NAME + 5));
	for (int i = 0; i < NUMBER_OF_INODES; i++){
		if (rootDirectory[i].i_node != -1)
		{
			strcpy(temp, rootDirectory[i].file_name);
			if (strcmp(temp, name) == 0){
				free(temp);
				return rootDirectory[i].i_node;
			}
		}
	}
	free(temp);
	return -1;
}

//get_index_bit: Get the index of the first free block in the bitmap.
//--------------------------------------------------------------------------------------------------------//
int get_index_bit() 
{
    int i = 0;
    while (bit_map[i] == 0) 
    { 
        i++; 
        if (i >= 128)
        {
            return 0;
        }
    }
    int bit = ffs(bit_map[i]) - 1;
    USE(bit_map[i], bit);
    return i*8 + bit;
}

//set_index_bit: Allocates space in the bit map for a given index. Mainly used for initialization purposes
//--------------------------------------------------------------------------------------------------------//
void set_index_bit(int index) 
{
    int i = index / 8;
    int bit = index % 8;
    //Force the bit to be free
    USE(bit_map[i], bit);
}

//remove_index_bit: removes the specified index from the bitmap and sets it as FREE
//--------------------------------------------------------------------------------------------------------//
void remove_index_bit(int index)
{
    int i = index / 8;
    int bit = index % 8;
    FREE(bit_map[i], bit);
}

//inode_finder: Finds the first available I-Node with linear search.
//--------------------------------------------------------------------------------------------------------//
int inode_finder()
{
	for (int i = 1; i < NUMBER_OF_INODES; i++)
	{
		if (iNodeTableStatus[i] == 0)
		{
			iNodeTableStatus[i] = 1;
			return i;
		}
	}
	return -1;
}

//mksfs: Formats the virtual disk implemented by the disk emulator and creates an instance of the
//simple file system on top of it.
//fresh = 1 --> The file system shall be create from scratch.
//fresh = 0 --> The file system is opened from the disk.
//--------------------------------------------------------------------------------------------------------//
void mksfs(int fresh)
{
	if(fresh == 1)
	//The file system should be created from scratch.
	{
		//Initialize File Descriptor Table first, setting their values to -1.
		for (int i = 0; i < NUMBER_OF_INODES; i++)
		{
			fdt[i].inode_number = -1;
			fdt[i].rwpointer = -1;
		}

		//Initialize the root directory, setting their values to -1.
		//Set poitners for the root directory
		int root_pointer[12];
		for (int i = 0; i < NUMBER_OF_INODES; i++)
		{
			rootDirectory[i].i_node = -1;
			for (int j = 0; j < 20; j++)
			{
				rootDirectory[i].file_name[j] = '0';
			}
		}	

		//Initialize the fresh disk, calling the provided funciton
		init_fresh_disk(SYSTEM_NAME, BLOCK_SIZE, NUMBER_OF_BLOCKS);

		//Initialize the I-Nodes, setting all their values to -1, representing they are empty.
		for (int i = 0; i < NUMBER_OF_INODES; i++)
		{
			iNodeTable[i].mode = -1;
			iNodeTable[i].link_count = -1;
			iNodeTable[i].uid = -1;
			iNodeTable[i].gid = -1;
			iNodeTable[i].size = -1;
			iNodeTable[i].indirect_pointers = -1;

			for (int j = 0; j < 12; j++){
				iNodeTable[i].pointers[j] = -1;
			}
			iNodeTableStatus[i] = 0;
		}


		//Find the number of blocks occupied by the inode table and the root directory respectively
		int inodeBlocks = (sizeof(iNodeTable)/BLOCK_SIZE);
		if (sizeof(iNodeTable) % BLOCK_SIZE != 0)
		{
			inodeBlocks += 1;
		}
		
		int rootDirectoryBlocks = (sizeof(rootDirectory)/BLOCK_SIZE);
		if (sizeof(rootDirectory) % BLOCK_SIZE != 0)
		{
			rootDirectoryBlocks += 1;
		}

		for(int i = 0; i < rootDirectoryBlocks; i++)
		{
			root_pointer[i] = i + 1 + inodeBlocks;
		}

		//Superblock has to be the first block in the SFS, create space for it with this function
		set_index_bit(0);
		for(int i = 1; i < inodeBlocks + 1; i ++)
		{
			//Create space for rest of the blocks
			set_index_bit(i);
		}

		for(int i = inodeBlocks+1; i < rootDirectoryBlocks + (inodeBlocks + 1); i++)
		{
			set_index_bit(i);
		}

		//Store the root dir and I-Node to 1022 and 1023
		set_index_bit(1022);
		set_index_bit(1023);
		iNodeTableStatus[0] = 1;
		iNodeTable[0].mode = 0;
		iNodeTable[0].link_count = rootDirectoryBlocks;
		iNodeTable[0].uid = 0;
		iNodeTable[0].gid = 0;
		iNodeTable[0].size = -1;
		iNodeTable[0].indirect_pointers = -1;

		for (int i = 0; i < 12; i++)
		{
			iNodeTable[0].pointers[i] = root_pointer[i];
		}	
		
		//Initialize the super block by setting the defined variables and write it in the disk
		superblock.magic = MAGIC;
		superblock.block_size = BLOCK_SIZE;
		superblock.file_system_size = NUMBER_OF_BLOCKS;
		superblock.inode_table_length = NUMBER_OF_INODES;
		superblock.root_directory = 0;

		directory_location = 0;//Used for tracking the current directory location.
		void *temp = malloc(BLOCK_SIZE * rootDirectoryBlocks);
		memcpy(temp, &rootDirectory, sizeof(rootDirectory));
		//Write the initialized blocks to disk
		write_blocks(0, 1, &superblock);
		write_blocks(1, inodeBlocks, &iNodeTable);
		write_blocks(iNodeTable[0].pointers[0], rootDirectoryBlocks, temp);
		write_blocks(1022, 1, &iNodeTableStatus);
		write_blocks(1023, 1, &bit_map);
		free(temp);
	}
	else 
	{
		//The file system exists.	
		for(int i = 0; i < NUMBER_OF_INODES; i++)
		{
			fdt[i].inode_number = -1;
		}

		init_disk(SYSTEM_NAME, BLOCK_SIZE, NUMBER_OF_BLOCKS);

		void* temp = malloc(BLOCK_SIZE);
		read_blocks(0, 1, temp);
		memcpy(&superblock, temp, sizeof(SuperBlock));
		directory_location = 0;//Used for tracking the current directory location.

		//Find the number of blocks occupied by the I-Node table and the root directory respectively
		int inodeBlocks = (sizeof(iNodeTable)/BLOCK_SIZE);
		if (sizeof(iNodeTable) % BLOCK_SIZE != 0)
		{
			inodeBlocks += 1;
		}
		//Refresh the buffer
		free(temp);
		temp = malloc(BLOCK_SIZE);
		read_blocks(1022, 1, temp);
		memcpy(&iNodeTableStatus, temp, sizeof(iNodeTableStatus));

		//Refresh the buffer
		free(temp);
		temp = malloc(BLOCK_SIZE);
		read_blocks(1023, 1, temp);
		memcpy(&bit_map, temp, sizeof(bit_map));

		//Refresh the buffer
		free(temp);
		temp = malloc(BLOCK_SIZE * (iNodeTable[0].link_count));
		read_blocks(iNodeTable[0].pointers[0], iNodeTable[0].link_count, temp);
		memcpy(&rootDirectory, temp, sizeof(rootDirectory));

		free(temp);
	}

}

//sfs_get_next_filename: Copies the name of the next file in the directory into fname and returns non 
//zero if there is a new file.
//--------------------------------------------------------------------------------------------------------//
int sfs_get_next_filename(char *fname){
	if (directory_location < NUMBER_OF_INODES){
		while (rootDirectory[directory_location].i_node == -1){
			directory_location++;
			if (directory_location >= NUMBER_OF_INODES){
				directory_location = 0;
				return 0;
			}
		}
		strcpy(fname, rootDirectory[directory_location].file_name);
		directory_location++;
		return 1; 
	} else {
		directory_location = 0;
		return 0;
	}

}

//sfs_GetFileSize: Returns the size of a given file
//--------------------------------------------------------------------------------------------------------//
int sfs_GetFileSize(const char* path)
{
	int iNodeId = get_directory_inode(path);
	if (iNodeId != -1){
		return iNodeTable[iNodeId].size;
	} else {
		return -1;
	}
}

//sfs_fopen: Opens a file and returns an integer which corresponds to the index of the entry for the
//newly opened file in the open file descriptor table.
//--------------------------------------------------------------------------------------------------------//
int sfs_fopen(char *name){

	//Check if the name exceeds the maximum allowed file name
	if (strlen(name) > MAXIMUM_FILE_NAME + 1){
		return -1;
	}
	//Find the first available entry in the file descriptor table
	int fileDescriptorIndex = -1;
	for(int i = 0; i < NUMBER_OF_INODES; i++)
	{
		if(fdt[i].inode_number == -1)
		{
			fileDescriptorIndex = i;
			break;
		}
	}

	if (fileDescriptorIndex != -1)
	{	
		//Find the I-Node associated
		int iNodeIndex = get_directory_inode(name);
		//Check if the file exists, if its open the file!
		if(iNodeIndex != -1)
		{
			for(int i = 0 ; i< NUMBER_OF_INODES; i++)
			{
				if(fdt[i].inode_number == iNodeIndex)
				{
					perror("Error opening the file: The file is already open!");
					return -1;
				}
				//Set the file descriptor paramters, place the write pointer to the end of the file
				fdt[fileDescriptorIndex].inode_number = iNodeIndex;
				fdt[fileDescriptorIndex].inode = &(iNodeTable[iNodeIndex]);
				//fdt[fileDescriptorIndex].rpointer = 0;
				//fdt[fileDescriptorIndex].wpointer = iNodeTable[iNodeIndex].size;
				fdt[fileDescriptorIndex].rwpointer = iNodeTable[iNodeIndex].size;
				
				return fileDescriptorIndex;
			}
		}

		//The file does not exists, we have to create it!
		else
		{
			// Pick an I-Node for it
			int newInodeIndex = inode_finder();
			if (newInodeIndex == -1){// No more free I-Nodes !
				return -1;
			}

			//Walk through the root directory and find the first available spot
			int directoryIndex = -1;
			for (int i = 0; i < NUMBER_OF_INODES; i++){
				if (rootDirectory[i].i_node == -1){
					directoryIndex = i;
				}
			}
			if (directoryIndex == -1)
			{
				perror("Error opening the file: Failed to create the file due to lack of space in the directory!");
				return -1;
			}

			//------------------------------//
			//Retrieve the data from bitmap
			int pointer = get_index_bit();
			if(pointer == 0)
			{
				return -1;
			}
			int pointers[12];
			pointers[0] = pointer;
			for(int i = 1; i < 12; i++)
			{
				pointers[i] = -1;
			}
			//------------------------------//
            
			fdt[fileDescriptorIndex].inode_number = newInodeIndex;
			rootDirectory[directoryIndex].i_node = newInodeIndex;
			strcpy(rootDirectory[directoryIndex].file_name, name);
			
			//Initialize the I-Node for the created file
			iNodeTableStatus[newInodeIndex] = 1;
			iNodeTable[newInodeIndex].mode = 0;
			iNodeTable[newInodeIndex].link_count = 1;
			iNodeTable[newInodeIndex].uid = 0;
			iNodeTable[newInodeIndex].gid = 0;
			iNodeTable[newInodeIndex].size = 0;
			iNodeTable[newInodeIndex].indirect_pointers = -1;

			for (int j = 0; j < 12; j++)
			{
				iNodeTable[newInodeIndex].pointers[j] = pointers[j];
			}
		
			fdt[fileDescriptorIndex].inode = &(iNodeTable[newInodeIndex]);
			fdt[fileDescriptorIndex].rwpointer = iNodeTable[newInodeIndex].size;

			int rootDirectoryBlock = (sizeof(rootDirectory)/BLOCK_SIZE);
            
			//Don't forget the offset
			if(sizeof(rootDirectory) % BLOCK_SIZE != 0)
			{
				rootDirectoryBlock = rootDirectoryBlock + 1;
			}
			iNodeTable[0].size = iNodeTable[0].size + 1;
			void *temp = malloc(BLOCK_SIZE * rootDirectoryBlock);
			memcpy(temp, &rootDirectory, sizeof(rootDirectory));

			int iNodeBlock = (sizeof(iNodeTable)/BLOCK_SIZE);
			if (sizeof(iNodeTable) % BLOCK_SIZE != 0)
			{
				iNodeBlock += 1;
			}

			//Write root directory to disk with write_blocks() function
			write_blocks(iNodeTable[0].pointers[0], rootDirectoryBlock, temp);
			free(temp);
            
			//Write I-Node table to disk with write_blocks() function
			write_blocks(1, iNodeBlock, &iNodeTable);

			//Write I-Node status to disk with write_blocks() function
			write_blocks(1022, 1, &iNodeTableStatus);

			//Write bitmap to disk with write_blocks() function
			write_blocks(1023, 1, &bit_map);

			return fileDescriptorIndex;
		}
	} else 
	{
		perror("Error opening file: No file descripter index available for specified file!\n");
		return -1;
	}
}

//sfs_fclose: closes a file, i.e., removes the entry from the open file descriptor table.
//--------------------------------------------------------------------------------------------------------//
int sfs_fclose(int fileID) {

	if (fdt[fileID].inode_number == -1)
	{
		return -1;
	} else 
	{
		fdt[fileID].inode_number = -1;
		fdt[fileID].rwpointer = -1;
		return 0;	
	}
}

//sfs_fread: Access in memory I-Node from file descriptor. I-node has info on the data blocks, find location
//of data block at current read pointer, fetch block from disk and read, need to handle cases of direct.
//--------------------------------------------------------------------------------------------------------//
int sfs_fread(int fileID, char *buf, int length) {

	//Check for valid file ID
	if (fileID < 0)
	{ 
		perror("Error reading file: Invalid fileID!\n");
		return -1;
	} 

	int iNodeId = fdt[fileID].inode_number;
	int rpointer = fdt[fileID].rwpointer;
	if (iNodeId == -1)
	{ 
		perror("Error reading file: Invalid directory!\n");
		return -1;
	}

	int startBlock = fdt[fileID].rwpointer / BLOCK_SIZE;
	int endBlock = (fdt[fileID].rwpointer + length) / BLOCK_SIZE;
	int offset = fdt[fileID].rwpointer % BLOCK_SIZE;

	//================================//
	//To dynamically keep track how much is read
	int amount; 
	int end;
	if (iNodeTable[iNodeId].size < (fdt[fileID].rwpointer + length))
	{
		amount = iNodeTable[iNodeId].size - fdt[fileID].rwpointer;
		end = iNodeTable[iNodeId].size / BLOCK_SIZE;

		if ((iNodeTable[iNodeId].size % BLOCK_SIZE) != 0) 
		{
			end = end + 1;
		}
	} 
	else 
	{
		amount = length;
		end = (fdt[fileID].rwpointer + length) / BLOCK_SIZE;
		if ((fdt[fileID].rwpointer + length) % BLOCK_SIZE != 0)
		 {
			end = end + 1;
		}
	}
	//================================//

	//Check for indirect pointers 
	void *temp1 = malloc(BLOCK_SIZE);
	if (iNodeTable[iNodeId].link_count > 12)
	{
		read_blocks(iNodeTable[iNodeId].indirect_pointers, 1, temp1);
		memcpy(&indirectPointerBlock, temp1, BLOCK_SIZE);
	}

	//Load the file into memory from each pointer
	//First allocate the space into a temporary buffer
	void *temp2 = malloc(BLOCK_SIZE * end);
	for (int i = startBlock ; i < iNodeTable[iNodeId].link_count && i < end; i++){
		if (i >= 12){
			read_blocks(indirectPointerBlock[i-12], 1, (temp2 + (i-startBlock) * BLOCK_SIZE));
		} else {
			read_blocks(iNodeTable[iNodeId].pointers[i], 1, (temp2 + (i-startBlock) * BLOCK_SIZE));
		}
	}
	//Set the read-write pointer to where we finished reading so if more
	//reading needs to be done in the future, while this file is open, it 
	//can continue from where it left off
	fdt[fileID].rwpointer += amount;

	//Extract whats read from the buffers and reset them for next use
	memcpy(buf, (temp2 + offset), amount);
	free(temp2);
	free(temp1);
    
	//Return the amount read
	return amount;
}

//sfs_fwrite: Access in memory inode from file descriptor. I-node has info on the data blocks, find location
//of data block at current write pointer, fetch block from disk and write, if needed, allocate new blocks
//from disk: use bitmap to get next free blocks to write on.
//--------------------------------------------------------------------------------------------------------//
int sfs_fwrite(int fileID, const char *buf, int length) 
{
	//Check for valid file ID
	if(fileID < 0)
	{
		perror("Error writing file: Invalid fileID!\n");
		return -1;
	}
	int iNodeId = fdt[fileID].inode_number;
	int wpointer = fdt[fileID].rwpointer;
	if (iNodeId == -1)
	{ 
		perror("Error writing file: Invalid directory!\n");
		return -1;
	}
	//Check if the additional content that will be written exceeds the given limit
	int need = wpointer + length; 
	int delta = 0;


	if (need > MAXIMUM_FILE_SIZE)
	{
		perror("Error writing file: File size cannot exceed 30000 bytes!\n");
		return -1;
	}

	double occupiedBlocks = iNodeTable[iNodeId].size/BLOCK_SIZE;
	double neededBlocks = need/BLOCK_SIZE;
	double extraBlocks = neededBlocks - occupiedBlocks;

	//int occupiedBlocks = iNodeTable[iNodeId].link_count; //This also works
	//Check for offset
	if (need % BLOCK_SIZE != 0){
		neededBlocks += 1;
	}

	//Check for indirect pointers - similar to read version
	void *temp1 = malloc(BLOCK_SIZE);
	if (iNodeTable[iNodeId].link_count > 12)
	{
		read_blocks(iNodeTable[iNodeId].indirect_pointers, 1, temp1);
		memcpy(&indirectPointerBlock, temp1, BLOCK_SIZE);
	} 
	else if (iNodeTable[iNodeId].link_count + extraBlocks > 12) 
	{
		//Retrieve the data from bitmap	
		int indirect = get_index_bit();
		if (indirect == 0){
			return -1;
		}		
		iNodeTable[iNodeId].indirect_pointers = indirect;
	}
	free(temp1);

	int startBlock = fdt[fileID].rwpointer / BLOCK_SIZE;
	int endBlock = neededBlocks;
	int offset = fdt[fileID].rwpointer % BLOCK_SIZE;
	//Assign the free blocks from bitmap if none available return error
	int newBlock = 0;
	if (extraBlocks > 0)
	{
		for (int i = iNodeTable[iNodeId].link_count; i < iNodeTable[iNodeId].link_count + extraBlocks; i++)
		{
			newBlock = get_index_bit();
			//Populate the indirect pointers for new blocks added
			if (newBlock != 0)
			{ 
				if (i >= 12)
				{
					indirectPointerBlock[i - 12] = newBlock;
				}
				 else 
				{
					iNodeTable[iNodeId].pointers[i] = newBlock;	
				}
			} 
			else
			{
				perror("Error writing file: No more block available!\n");
				return -1;
			}
		}
	} 
	else
	{
		extraBlocks = 0;
	}

	//Load the file into memory from each pointer:
	void *temp2 = malloc(BLOCK_SIZE*endBlock);
	//Iterate from start block to end block and also include link counts
	for (int i = startBlock; i < iNodeTable[iNodeId].link_count && i < endBlock; i++)
	{
		if (i >= 12)
		{
			read_blocks(indirectPointerBlock[i-12], 1, (temp2 + (i-startBlock) * BLOCK_SIZE));
		} 
		else
		{
			read_blocks(iNodeTable[iNodeId].pointers[i], 1, (temp2 + (i-startBlock) * BLOCK_SIZE));
		}
	}

	memcpy((temp2 + offset), buf, length);

	//Update the disk
	write_blocks(1022, 1, &iNodeTableStatus);

	//Write the poitners
	for (int i = startBlock; i < endBlock; i++){
		if (i >= 12){
			write_blocks(indirectPointerBlock[i-12], 1, (temp2 + (i-startBlock) * BLOCK_SIZE));
		} else {
			write_blocks(iNodeTable[iNodeId].pointers[i], 1, (temp2 + ((i-startBlock) * BLOCK_SIZE)));
		}
	}
	//Update the inode
	if (iNodeTable[iNodeId].size < need)
	{
		iNodeTable[iNodeId].size = need;
	} 
	iNodeTable[iNodeId].link_count += extraBlocks;
	fdt[fileID].rwpointer = need;
	if (iNodeTable[iNodeId].link_count > 12){
		write_blocks(iNodeTable[iNodeId].indirect_pointers, 1, &indirectPointerBlock);
	}
	//Write the I-Node table to disk
	//Find the number of blocks occupied by the I-Node table
	int inodeBlocks = (sizeof(iNodeTable)/BLOCK_SIZE);
	if (sizeof(iNodeTable) % BLOCK_SIZE != 0)
	{
			inodeBlocks += 1;
	}
	write_blocks(1, inodeBlocks, &iNodeTable);

	// Write the I-Node status to disk
	write_blocks(1022, 1, &iNodeTableStatus);

	// Write bitmap to disk
	write_blocks(1023, 1, &bit_map);

	free(temp2);
	//Return bytes written
	return length;
}

//sfs_frseek: Moves the writer pointer to the given location.
//--------------------------------------------------------------------------------------------------------//
int sfs_fwseek(int fileID, int loc) {
	if (iNodeTable[fdt[fileID].inode_number].size < loc)
	{ 
		perror("Error wseeking!");
		return -1;
	}
	//Write always continues from the end of the file
	//fdt[fileID].rwpointer = iNodeTable[fdt[fileID].inode_number].size;
	fdt[fileID].rwpointer = loc;
	return 0;
}

//sfs_frseek: Moves the read pointer to the given location.
//--------------------------------------------------------------------------------------------------------//
int sfs_frseek(int fileID, int loc) {
	if (iNodeTable[fdt[fileID].inode_number].size < loc)
	{ 
		perror("Error rseeking!");
		return -1;
	}
	//Read always start from beginning of the file
	fdt[fileID].rwpointer = 0;
	return 0;
}

//sfs_remove: Removes the file from the directory entry, releases the file allocation table entries and 
//releases the data blocks used by the file, so that they can be used by new files in the future.
//--------------------------------------------------------------------------------------------------------//
int sfs_remove(char *file) {
	void *temp = malloc(BLOCK_SIZE);
	int iNodeId = get_directory_inode(file);
	if (iNodeId > 0)
	 {
		//Free up the bitmap
		for (int i = 0; i < iNodeTable[iNodeId].link_count && i < 12; i++)
		{
			remove_index_bit(iNodeTable[iNodeId].pointers[i]);
		}

		 //Check for indirect pointers
		if (iNodeTable[iNodeId].link_count > 12)
		{
			read_blocks(iNodeTable[iNodeId].indirect_pointers, 1, temp);
			memcpy(&indirectPointerBlock, temp, BLOCK_SIZE);

			//Free the bitmap
			for (int i = 12; i < iNodeTable[iNodeId].link_count; i++)
			{
				remove_index_bit(indirectPointerBlock[i-12]);
			}
			free(temp);
		}

		//Remove I-Node contents, -1 --> d.n.e.
		iNodeTableStatus[iNodeId] = 0;
		iNodeTable[iNodeId].mode = -1;
		iNodeTable[iNodeId].link_count = -1;
		iNodeTable[iNodeId].uid = -1;
		iNodeTable[iNodeId].gid = -1;
		iNodeTable[iNodeId].size = -1;
		iNodeTable[iNodeId].indirect_pointers = -1;
		iNodeTable[iNodeId].uid = -1;
		//Similarly assign I-Node pointers to be -1
		for (int j = 0; j < 12; j++){
			iNodeTable[iNodeId].pointers[j] = -1;
		}

		for (int i = 0; i < NUMBER_OF_INODES; i++){
			if (strcmp(rootDirectory[i].file_name, file) == 0){
				rootDirectory[i].i_node = -1;
				for (int j = 0; j < MAXIMUM_FILE_NAME; j++){
					rootDirectory[i].file_name[0]='0';
				}
				break;
			}
		}

		iNodeTable[0].size -= 1;

		int rootDirectoryBlocks = (sizeof(rootDirectory)/BLOCK_SIZE);
		if (sizeof(rootDirectory) % BLOCK_SIZE != 0){
			rootDirectoryBlocks += 1;
		}

		void *temp2 = malloc(BLOCK_SIZE * rootDirectoryBlocks);
		memcpy(temp2, &rootDirectory, sizeof(rootDirectory));
		write_blocks(iNodeTable[0].pointers[0], rootDirectoryBlocks, temp2);
		free(temp2);

		int numInodeBlocks = (sizeof(iNodeTable)/BLOCK_SIZE);
		if (sizeof(iNodeTable) % BLOCK_SIZE != 0)
		{
			numInodeBlocks = numInodeBlocks + 1;
		}
		write_blocks(1, numInodeBlocks, &iNodeTable);
		write_blocks(1022, 1, &iNodeTableStatus);
		write_blocks(1023, 1, &bit_map);
		return 0;
	} 
	else 
	{
		perror("Error removing file: File does not exist!\n");
		return -1;
	}
}