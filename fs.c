/* 
 * File System Layer
 * 
 * fs.c
 *
 * Implementation of the file system layer. Manages the internal 
 * organization of files and directories in a 'virtual memory disk'.
 * Implements the interface functions specified in fs.h.
 *
 */


#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "fs.h"
#include <time.h>          // Para time_t
#include <pthread.h>       // Para pthread_mutex_*
#include <string.h>        // Para memcpy, memset
#include <stdlib.h>        // Para malloc, free

/* Novas directivas */
#define BLOCK_CACHE_SIZE 10
#define INODE_CACHE_SIZE 4
#define DIR_CACHE_SIZE 4


#define dprintf if(1) printf

#define BLOCK_SIZE 512

#define INODE_NUM_BLKS 10

#define EXT_INODE_NUM_BLKS (BLOCK_SIZE / sizeof(unsigned int))

typedef struct fs_inode {
    fs_itype_t type;
    unsigned int size;
    unsigned int blocks[INODE_NUM_BLKS];
    unsigned int reserved[4]; // reserved[0] -> extending table block number
 } fs_inode_t;

typedef struct {
    unsigned int block_num;
    char data[BLOCK_SIZE];
    int dirty;
    time_t last_access;
} block_cache_entry_t;

typedef struct {
    inodeid_t inode_num;
    fs_inode_t inode;
    int dirty;
    time_t last_access;
} inode_cache_entry_t;

#define DIR_PAGE_ENTRIES (BLOCK_SIZE / sizeof(fs_dentry_t))

typedef struct dentry {
   char name[FS_MAX_FNAME_SZ];
   inodeid_t inodeid;
} fs_dentry_t;

typedef struct {
    inodeid_t dir_num;
    unsigned int block_num;
    fs_dentry_t entries[DIR_PAGE_ENTRIES];
    time_t last_access;
} dir_cache_entry_t;

/*
 * Inode
 * - inode size = 64 bytes
 * - num of direct block refs = 10 blocks
 */

typedef unsigned int fs_inode_ext_t;


/*
 * Directory entry
 * - directory entry size = 16 bytes
 * - filename max size - 14 bytes (13 chars + '\0') defined in fs.h
 */


/*
 * File syste structure
 * - inode table size = 64 entries (8 blocks)
 * 
 * Internal organization 
 *   - block 0        - free block bitmap
 *   - block 1        - free inode bitmap
 *   - block 2-9      - inode table (8 blocks)
 *   - block 10-(N-1) - data blocks, where N is the number of blocks
 */

#define ITAB_NUM_BLKS 8

#define ITAB_SIZE (ITAB_NUM_BLKS*BLOCK_SIZE / sizeof(fs_inode_t))

struct fs_ {
    /* Componentes originais (sem duplicação) */
    blocks_t* blocks;               // Ponteiro para os blocos do dispositivo
    char inode_bmap[BLOCK_SIZE];    // Bitmap de inodes livres
    char blk_bmap[BLOCK_SIZE];      // Bitmap de blocos livres
    fs_inode_t inode_tab[ITAB_SIZE]; // Tabela de inodes
 
    /* Novos campos para o sistema de cache */
    block_cache_entry_t block_cache[BLOCK_CACHE_SIZE];  // Cache de blocos
    inode_cache_entry_t inode_cache[INODE_CACHE_SIZE];  // Cache de inodes
    dir_cache_entry_t dir_cache[DIR_CACHE_SIZE];        // Cache de diretórios
    pthread_mutex_t cache_mutex;    // Mutex para sincronização
 };

#define NOT_FS_INITIALIZER  1
                               
/*
 * Internal functions for loading/storing file system metadata do the blocks
 */

/* Funções auxiliares da cache */
// Funções para encontrar/inserir em cada cache
static block_cache_entry_t* find_block_in_cache(fs_t* fs, unsigned int block_num) {
    for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
        if (fs->block_cache[i].block_num == block_num) {
            fs->block_cache[i].last_access = time(NULL);
            return &fs->block_cache[i];
        }
    }
    return NULL;
}

// Adiciona um bloco à cache usando política LRU
static void add_block_to_cache(fs_t* fs, unsigned int block_num, char* data, int dirty) {
    // Encontrar entrada LRU
    int lru_index = 0;
    time_t lru_time = fs->block_cache[0].last_access;
    
    for (int i = 1; i < BLOCK_CACHE_SIZE; i++) {
        if (fs->block_cache[i].last_access < lru_time) {
            lru_index = i;
            lru_time = fs->block_cache[i].last_access;
        }
    }
    
    // Se a entrada LRU estiver dirty, escrever de volta
    if (fs->block_cache[lru_index].dirty) {
        block_write(fs->blocks, fs->block_cache[lru_index].block_num, 
                   fs->block_cache[lru_index].data);
    }
    
    // Adicionar novo bloco à cache
    fs->block_cache[lru_index].block_num = block_num;
    memcpy(fs->block_cache[lru_index].data, data, BLOCK_SIZE);
    fs->block_cache[lru_index].dirty = dirty;
    fs->block_cache[lru_index].last_access = time(NULL);
}


// Funções similares para inode_cache e dir_cache

// Substituir chamadas diretas a block_read/block_write por funções que usam cache

static int __attribute__((unused)) cached_block_read(fs_t* fs, unsigned int block_num, char* buffer) {
    pthread_mutex_lock(&fs->cache_mutex);
    
    // Verificar se está na cache
    block_cache_entry_t* cached = find_block_in_cache(fs, block_num);
    if (cached) {
        memcpy(buffer, cached->data, BLOCK_SIZE);
        pthread_mutex_unlock(&fs->cache_mutex);
        return 0;
    }
    
    // Se não estiver na cache, ler do disco
    int res = block_read(fs->blocks, block_num, buffer);
    
    // Adicionar à cache
    add_block_to_cache(fs, block_num, buffer, 0);
    
    pthread_mutex_unlock(&fs->cache_mutex);
    return res;
}

static int __attribute__((unused)) cached_block_write(fs_t* fs, unsigned int block_num, char* data) {
    pthread_mutex_lock(&fs->cache_mutex);
    
    // Atualizar cache se o bloco estiver lá
    block_cache_entry_t* cached = find_block_in_cache(fs, block_num);
    if (cached) {
        memcpy(cached->data, data, BLOCK_SIZE);
        cached->dirty = 1;
        cached->last_access = time(NULL);
        pthread_mutex_unlock(&fs->cache_mutex);
        return 0; // Write-back: não escreve imediatamente no disco
    }
    
    // Se não estiver na cache, adicionar
    add_block_to_cache(fs, block_num, data, 1);
    
    pthread_mutex_unlock(&fs->cache_mutex);
    return 0;
}                          
                                
static void fsi_load_fsdata(fs_t* fs)
{
   blocks_t* bks = fs->blocks;
   
   // load free block bitmap from block 0
   block_read(bks,0,fs->blk_bmap);

   // load free inode bitmap from block 1
   block_read(bks,1,fs->inode_bmap);
   
   // load inode table from blocks 2-9
   for (int i = 0; i < ITAB_NUM_BLKS; i++) {
      block_read(bks,i+2,&((char*)fs->inode_tab)[i*BLOCK_SIZE]);
   }
#define NOT_FS_INITIALIZER  1  //file system is already initialized, subsequent block acess will be delayed using a sleep function.
}


static void fsi_store_fsdata(fs_t* fs)
{
   blocks_t* bks = fs->blocks;
 
   // store free block bitmap to block 0
   block_write(bks,0,fs->blk_bmap);

   // store free inode bitmap to block 1
   block_write(bks,1,fs->inode_bmap);
   
   // store inode table to blocks 2-9
   for (int i = 0; i < ITAB_NUM_BLKS; i++) {
      block_write(bks,i+2,&((char*)fs->inode_tab)[i*BLOCK_SIZE]);
   }
}


/*
 * Bitmap management macros and functions
 */

#define BMAP_SET(bmap,num) ((bmap)[(num)/8]|=(0x1<<((num)%8)))

#define BMAP_CLR(bmap,num) ((bmap)[(num)/8]&=~((0x1<<((num)%8))))

#define BMAP_ISSET(bmap,num) ((bmap)[(num)/8]&(0x1<<((num)%8)))


static int fsi_bmap_find_free(char* bmap, int size, unsigned* free)
{
   for (int i = 0; i < size; i++) {
      if (!BMAP_ISSET(bmap,i)) {
         *free = i;
         return 1;
      }
   }
   return 0;
}


static void fsi_dump_bmap(char* bmap, int size)
{
   int i = 0;
   for (; i < size; i++) {
      printf("%x.", (unsigned char)bmap[i]);
      if (i > 0 && i % 32 == 0) {
         printf("\n");
      }
   }
}


/*
 * Other internal file system macros and functions
 */

#define MIN(a,b) ((a)<=(b)?(a):(b))
                                
#define MAX(a,b) ((a)>=(b)?(a):(b))
                                
#define OFFSET_TO_BLOCKS(pos) ((pos)/BLOCK_SIZE+(((pos)%BLOCK_SIZE>0)?1:0))

                                
static void fsi_inode_init(fs_inode_t* inode, fs_itype_t type)
{
   int i;
   
   inode->type = type;
   inode->size = 0;
   for (i = 0; i < INODE_NUM_BLKS; i++) {
      inode->blocks[i] = 0;
   }
   
   for (i = 0; i < 4; i++) {
	   inode->reserved[i] = 0;
   }
}


static int fsi_dir_search(fs_t* fs, inodeid_t dir, char* file, 
   inodeid_t* fileid)
{
    if (fs == NULL || file == NULL || fileid == NULL) {
        dprintf("[fsi_dir_search] malformed arguments.\n");
        return -1;
    }

    pthread_mutex_lock(&fs->cache_mutex);
    
    // 1. Tentar obter da cache de diretorias
    dir_cache_entry_t* cached_dir = NULL;
    fs_inode_t* idir = &fs->inode_tab[dir];
    int found_in_cache = 0;
    
    // Procurar por todas as entradas de cache deste diretório
    for (int i = 0; i < DIR_CACHE_SIZE; i++) {
        if (fs->dir_cache[i].dir_num == dir) {
            cached_dir = &fs->dir_cache[i];
            cached_dir->last_access = time(NULL); // Atualiza LRU
            
            // Procurar o arquivo nas entradas em cache
            for (int j = 0; j < DIR_PAGE_ENTRIES; j++) {
                if (strcmp(cached_dir->entries[j].name, file) == 0) {
                    *fileid = cached_dir->entries[j].inodeid;
                    pthread_mutex_unlock(&fs->cache_mutex);
                    return 0;
                }
            }
            found_in_cache = 1;
            break;
        }
    }
    
    pthread_mutex_unlock(&fs->cache_mutex);

    // 2. Se não encontrou completo na cache, pesquisar no disco
    fs_dentry_t page[DIR_PAGE_ENTRIES];
    int num = idir->size / sizeof(fs_dentry_t);
    int iblock = 0;
    int cache_updated = 0;
    
    while (num > 0) {
        unsigned int block_num = idir->blocks[iblock];
        fs_dentry_t* current_page = NULL;
        
        pthread_mutex_lock(&fs->cache_mutex);
        
        // Verificar se este bloco específico está em cache
        int block_cached = 0;
        for (int i = 0; i < DIR_CACHE_SIZE; i++) {
            if (fs->dir_cache[i].dir_num == dir && 
                fs->dir_cache[i].block_num == block_num) {
                current_page = fs->dir_cache[i].entries;
                fs->dir_cache[i].last_access = time(NULL);
                block_cached = 1;
                break;
            }
        }
        
        if (!block_cached) {
            // Se o bloco não está em cache, ler do disco
            pthread_mutex_unlock(&fs->cache_mutex);
            if (block_read(fs->blocks, block_num, (char*)page)) {
                dprintf("[fsi_dir_search] error reading block %d\n", block_num);
                return -1;
            }
            current_page = page;
            
            // Adicionar à cache (substituição LRU)
            pthread_mutex_lock(&fs->cache_mutex);
            int lru_index = 0;
            time_t lru_time = fs->dir_cache[0].last_access;
            
            for (int i = 1; i < DIR_CACHE_SIZE; i++) {
                if (fs->dir_cache[i].last_access < lru_time) {
                    lru_index = i;
                    lru_time = fs->dir_cache[i].last_access;
                }
            }
            
            // Atualizar cache
            fs->dir_cache[lru_index].dir_num = dir;
            fs->dir_cache[lru_index].block_num = block_num;
            memcpy(fs->dir_cache[lru_index].entries, current_page, BLOCK_SIZE);
            fs->dir_cache[lru_index].last_access = time(NULL);
            cache_updated = 1;
        }
        
        // Procurar o arquivo no bloco atual
        for (int i = 0; i < DIR_PAGE_ENTRIES && num > 0; i++, num--) {
            if (strcmp(current_page[i].name, file) == 0) {
                *fileid = current_page[i].inodeid;
                
                // Se encontrou e não estava na cache completa, atualizar
                if (!found_in_cache && cache_updated) {
                    pthread_mutex_unlock(&fs->cache_mutex);
                    return 0;
                }
                
                // Se estava parcialmente em cache, garantir consistência
                if (found_in_cache) {
                    // Adicionar esta entrada à cache existente
                    for (int j = 0; j < DIR_PAGE_ENTRIES; j++) {
                        if (cached_dir->entries[j].name[0] == '\0') {
                            memcpy(&cached_dir->entries[j], &current_page[i], sizeof(fs_dentry_t));
                            break;
                        }
                    }
                }
                
                pthread_mutex_unlock(&fs->cache_mutex);
                return 0;
            }
        }
        
        pthread_mutex_unlock(&fs->cache_mutex);
        iblock++;
    }
    
    return -1; // Arquivo não encontrado
}


/*
 * File system interface functions
 */


void io_delay_on(int disk_delay);

fs_t* fs_new(unsigned num_blocks, int disk_delay)
{
    io_delay_on(disk_delay);
    
    fs_t* fs = (fs_t*)malloc(sizeof(fs_t));
    if (!fs) {
        printf("[fs_new] Error allocating filesystem structure\n");
        return NULL;
    }

    // Inicializa o mutex
    if (pthread_mutex_init(&fs->cache_mutex, NULL) != 0) {
        printf("[fs_new] Error initializing cache mutex\n");
        free(fs);
        return NULL;
    }

    // Inicializa o dispositivo de blocos
    fs->blocks = block_new(num_blocks, BLOCK_SIZE);
    if (!fs->blocks) {
        printf("[fs_new] Error creating block device\n");
        pthread_mutex_destroy(&fs->cache_mutex);
        free(fs);
        return NULL;
    }

    // Inicializa caches
    memset(fs->block_cache, 0, sizeof(fs->block_cache));
    memset(fs->inode_cache, 0, sizeof(fs->inode_cache));
    memset(fs->dir_cache, 0, sizeof(fs->dir_cache));

    // Carrega metadados
    fsi_load_fsdata(fs);
    
    return fs;
}


int fs_format(fs_t* fs)
{
   if (fs == NULL) {
      printf("[fs] argument is null.\n");
      return -1;
   }

   // erase all blocks
   char null_block[BLOCK_SIZE];
   memset(null_block,0,sizeof(null_block));
   for (int i = 0; i < block_num_blocks(fs->blocks); i++) {
      block_write(fs->blocks,i,null_block);
   }

   // reserve file system meta data blocks
   BMAP_SET(fs->blk_bmap,0);
   BMAP_SET(fs->blk_bmap,1);
   for (int i = 0; i < ITAB_NUM_BLKS; i++) {
      BMAP_SET(fs->blk_bmap,i+2);
   }

   // reserve inodes 0 (will never be used) and 1 (the root)
   BMAP_SET(fs->inode_bmap,0);
   BMAP_SET(fs->inode_bmap,1);
   fsi_inode_init(&fs->inode_tab[1],FS_DIR);

   // save the file system metadata
   fsi_store_fsdata(fs);
   return 0;
}

/* Funções auxiliares para fs_get_attrs */
// Encontra um inode na cache
static inode_cache_entry_t* find_inode_in_cache(fs_t* fs, inodeid_t inode_num) {
    for (int i = 0; i < INODE_CACHE_SIZE; i++) {
        if (fs->inode_cache[i].inode_num == inode_num) {
            return &fs->inode_cache[i];
        }
    }
    return NULL;
}

// Adiciona um inode à cache usando política LRU
static void add_inode_to_cache(fs_t* fs, inodeid_t inode_num, fs_inode_t* inode, int dirty) {
    // Encontrar entrada LRU
    int lru_index = 0;
    time_t lru_time = fs->inode_cache[0].last_access;
    
    for (int i = 1; i < INODE_CACHE_SIZE; i++) {
        if (fs->inode_cache[i].last_access < lru_time) {
            lru_index = i;
            lru_time = fs->inode_cache[i].last_access;
        }
    }
    
    // Se a entrada LRU estiver dirty, escrever de volta
    if (fs->inode_cache[lru_index].dirty) {
        fs->inode_tab[fs->inode_cache[lru_index].inode_num] = fs->inode_cache[lru_index].inode;
        // Nota: Não chamamos fsi_store_fsdata() aqui para evitar escrita desnecessária
    }
    
    // Adicionar novo inode à cache
    fs->inode_cache[lru_index].inode_num = inode_num;
    fs->inode_cache[lru_index].inode = *inode;
    fs->inode_cache[lru_index].dirty = dirty;
    fs->inode_cache[lru_index].last_access = time(NULL);
}


int fs_get_attrs(fs_t* fs, inodeid_t file, fs_file_attrs_t* attrs)
{
    if (fs == NULL || file >= ITAB_SIZE || attrs == NULL) {
        dprintf("[fs_get_attrs] malformed arguments.\n");
        return -1;
    }

    pthread_mutex_lock(&fs->cache_mutex);
    
    // 1. Tentar obter da cache de inodes
    inode_cache_entry_t* cached_inode = find_inode_in_cache(fs, file);
    fs_inode_t* inode = NULL;
    
    if (cached_inode) {
        // Encontrado na cache - atualizar LRU
        cached_inode->last_access = time(NULL);
        inode = &cached_inode->inode;
    } else {
        // Não está na cache - verificar bitmap e obter da tabela principal
        if (!BMAP_ISSET(fs->inode_bmap, file)) {
            pthread_mutex_unlock(&fs->cache_mutex);
            dprintf("[fs_get_attrs] inode is not being used.\n");
            return -1;
        }
        inode = &fs->inode_tab[file];
        
        // Adicionar à cache (não dirty pois só estamos lendo)
        add_inode_to_cache(fs, file, inode, 0);
    }
    
    // 2. Preencher a estrutura de atributos
    attrs->inodeid = file;
    attrs->type = inode->type;
    attrs->size = inode->size;
    
    switch (inode->type) {
        case FS_DIR:
            attrs->num_entries = inode->size / sizeof(fs_dentry_t);
            break;
        case FS_FILE:
            attrs->num_entries = -1;
            break;
        default:
            pthread_mutex_unlock(&fs->cache_mutex);
            dprintf("[fs_get_attrs] fatal error - invalid inode.\n");
            exit(-1);
    }
    
    pthread_mutex_unlock(&fs->cache_mutex);
    return 0;
}


int fs_lookup(fs_t* fs, char* file, inodeid_t* fileid)
{

char *token;
char line[MAX_PATH_NAME_SIZE]; 
char *search = "/";
int i=0;
int dir=0;
   if (fs==NULL || file==NULL ) {
      dprintf("[fs_lookup] malformed arguments.\n");
      return -1;
   }


    if(file[0] != '/') {
        dprintf("[fs_lookup] malformed pathname.\n");
        return -1;
    }

    strcpy(line,file);
    token = strtok(line, search);
    
   while(token != NULL) {
     i++;
     if(i==1) dir=1;  //Root directory
     
     if (!BMAP_ISSET(fs->inode_bmap,dir)) {
	      dprintf("[fs_lookup] inode is not being used.\n");
	      return -1;
     }
     fs_inode_t* idir = &fs->inode_tab[dir];
     if (idir->type != FS_DIR) {
        dprintf("[fs_lookup] inode is not a directory.\n");
        return -1;
     }
     inodeid_t fid;
     if (fsi_dir_search(fs,dir,token,&fid) < 0) {
        dprintf("[fs_lookup] file does not exist.\n");
        return 0;
     }
     *fileid = fid;
     dir=fid;
     token = strtok(NULL, search);
   }

   return 1;
}

int fs_read(fs_t* fs, inodeid_t file, unsigned offset, unsigned count, 
   char* buffer, int* nread)
{
    if (fs == NULL || file >= ITAB_SIZE || buffer == NULL || nread == NULL) {
        dprintf("[fs_read] malformed arguments.\n");
        return -1;
    }

    // Verificar cache de inodes primeiro
    pthread_mutex_lock(&fs->cache_mutex);
    fs_inode_t* ifile = NULL;
    inode_cache_entry_t* cached_inode = find_inode_in_cache(fs, file);
    
    if (cached_inode) {
        ifile = &cached_inode->inode;
        cached_inode->last_access = time(NULL); // Atualiza LRU
    } else {
        // Se não estiver na cache, buscar da tabela de inodes
        if (!BMAP_ISSET(fs->inode_bmap, file)) {
            pthread_mutex_unlock(&fs->cache_mutex);
            dprintf("[fs_read] inode is not being used.\n");
            return -1;
        }
        ifile = &fs->inode_tab[file];
        
        // Adicionar inode à cache
        add_inode_to_cache(fs, file, ifile, 0);
    }
    pthread_mutex_unlock(&fs->cache_mutex);

    if (ifile->type != FS_FILE) {
        dprintf("[fs_read] inode is not a file.\n");
        return -1;
    }

    if (offset >= ifile->size) {
        *nread = 0;
        return 0;
    }
    
    // Calcular quantidade máxima que pode ser lida
    int max = MIN(count, ifile->size - offset);
    int pos = 0;
    int iblock = offset / BLOCK_SIZE;
    int blks_used = OFFSET_TO_BLOCKS(ifile->size);
    
    while (pos < max && iblock < blks_used) {
        unsigned int block_num;
        if (iblock < INODE_NUM_BLKS) {
            block_num = ifile->blocks[iblock];
        } else {
            // Lidar com blocos indiretos (se implementado)
            pthread_mutex_unlock(&fs->cache_mutex);
            dprintf("[fs_read] indirect blocks not supported.\n");
            return -1;
        }

        char block_data[BLOCK_SIZE];
        
        // Tentar obter da cache de blocos
        pthread_mutex_lock(&fs->cache_mutex);
        block_cache_entry_t* cached_block = find_block_in_cache(fs, block_num);
        
        if (cached_block) {
            memcpy(block_data, cached_block->data, BLOCK_SIZE);
            cached_block->last_access = time(NULL); // Atualiza LRU
            pthread_mutex_unlock(&fs->cache_mutex);
        } else {
            pthread_mutex_unlock(&fs->cache_mutex);
            
            // Se não estiver na cache, ler do disco
            if (block_read(fs->blocks, block_num, block_data)) {
                dprintf("[fs_read] error reading block %d\n", block_num);
                return -1;
            }
            
            // Adicionar à cache (apenas leitura, não dirty)
            pthread_mutex_lock(&fs->cache_mutex);
            add_block_to_cache(fs, block_num, block_data, 0);
            pthread_mutex_unlock(&fs->cache_mutex);
        }

        // Copiar dados para o buffer do usuário
        int start = (pos == 0) ? (offset % BLOCK_SIZE) : 0;
        int num = MIN(BLOCK_SIZE - start, max - pos);
        memcpy(&buffer[pos], &block_data[start], num);
        
        pos += num;
        iblock++;
    }
    
    *nread = pos;
    return 0;
}


int fs_write(fs_t* fs, inodeid_t file, unsigned offset, unsigned count,
   char* buffer)
{
    if (fs == NULL || file >= ITAB_SIZE || buffer == NULL) {
        dprintf("[fs_write] malformed arguments.\n");
        return -1;
    }

    // 1. Verificar e obter o inode (usando cache)
    pthread_mutex_lock(&fs->cache_mutex);
    fs_inode_t* ifile = NULL;
    inode_cache_entry_t* cached_inode = find_inode_in_cache(fs, file);
    
    if (cached_inode) {
        ifile = &cached_inode->inode;
        cached_inode->last_access = time(NULL); // Atualiza LRU
    } else {
        if (!BMAP_ISSET(fs->inode_bmap, file)) {
            pthread_mutex_unlock(&fs->cache_mutex);
            dprintf("[fs_write] inode is not being used.\n");
            return -1;
        }
        ifile = &fs->inode_tab[file];
        add_inode_to_cache(fs, file, ifile, 0); // Adiciona à cache
    }
    
    if (ifile->type != FS_FILE) {
        pthread_mutex_unlock(&fs->cache_mutex);
        dprintf("[fs_write] inode is not a file.\n");
        return -1;
    }

    if (offset > ifile->size) {
        offset = ifile->size;
    }

    // 2. Calcular blocos necessários
    int blks_used = OFFSET_TO_BLOCKS(ifile->size);
    int blks_req = MAX(OFFSET_TO_BLOCKS(offset + count), blks_used) - blks_used;
    
    dprintf("[fs_write] count=%d, offset=%d, fsize=%d, bused=%d, breq=%d\n",
        count, offset, ifile->size, blks_used, blks_req);
    
    // 3. Alocar novos blocos se necessário
    if (blks_req > 0) {
        if (blks_req > INODE_NUM_BLKS - blks_used) {
            pthread_mutex_unlock(&fs->cache_mutex);
            dprintf("[fs_write] no free block entries in inode.\n");
            return -1;
        }

        dprintf("[fs_write] required %d blocks, used %d\n", blks_req, blks_used);

        // Alocar e reservar novos blocos
        for (int i = blks_used; i < blks_used + blks_req; i++) {
            unsigned int block_num;
            
            if (!fsi_bmap_find_free(fs->blk_bmap, block_num_blocks(fs->blocks), &block_num)) {
                pthread_mutex_unlock(&fs->cache_mutex);
                dprintf("[fs_write] there are no free blocks.\n");
                return -1;
            }
            
            BMAP_SET(fs->blk_bmap, block_num);
            ifile->blocks[i] = block_num;
            dprintf("[fs_write] block %d allocated.\n", block_num);
            
            // Adicionar novo bloco à cache (vazio)
            char empty_block[BLOCK_SIZE] = {0};
            add_block_to_cache(fs, block_num, empty_block, 1); // Já marca como dirty
        }
        
        // Marcar inode como modificado
        if (cached_inode) {
            cached_inode->dirty = 1;
        } else {
            // Se não estava na cache, atualizar na tabela principal
            fs->inode_tab[file] = *ifile;
        }
    }
    
    pthread_mutex_unlock(&fs->cache_mutex);

    // 4. Escrever os dados nos blocos (usando cache)
    int num = 0;
    int iblock = offset / BLOCK_SIZE;
    
    while (num < count) {
        unsigned int block_num = ifile->blocks[iblock];
        char block_data[BLOCK_SIZE];
        
        // 4.1 Obter o bloco (da cache ou disco)
        pthread_mutex_lock(&fs->cache_mutex);
        block_cache_entry_t* cached_block = find_block_in_cache(fs, block_num);
        
        if (cached_block) {
            memcpy(block_data, cached_block->data, BLOCK_SIZE);
        } else {
            // Se não está na cache, ler do disco
            pthread_mutex_unlock(&fs->cache_mutex);
            if (block_read(fs->blocks, block_num, block_data)) {
                dprintf("[fs_write] error reading block %d\n", block_num);
                return -1;
            }
            pthread_mutex_lock(&fs->cache_mutex);
        }
        
        // 4.2 Modificar o bloco
        int start = (num == 0) ? (offset % BLOCK_SIZE) : 0;
        int to_write = MIN(BLOCK_SIZE - start, count - num);
        
        memcpy(&block_data[start], &buffer[num], to_write);
        num += to_write;
        iblock++;
        
        // 4.3 Atualizar cache (marcar como dirty)
        if (cached_block) {
            memcpy(cached_block->data, block_data, BLOCK_SIZE);
            cached_block->dirty = 1;
            cached_block->last_access = time(NULL);
        } else {
            add_block_to_cache(fs, block_num, block_data, 1);
        }
        
        pthread_mutex_unlock(&fs->cache_mutex);
    }

    // 5. Atualizar tamanho do arquivo se necessário
    if (offset + count > ifile->size) {
        pthread_mutex_lock(&fs->cache_mutex);
        
        ifile->size = offset + count;
        
        // Marcar inode como modificado
        if (cached_inode) {
            cached_inode->dirty = 1;
        } else {
            fs->inode_tab[file] = *ifile;
            // Adicionar à cache agora que foi modificado
            add_inode_to_cache(fs, file, ifile, 1);
        }
        
        pthread_mutex_unlock(&fs->cache_mutex);
    }

    dprintf("[fs_write] written %d bytes, file size %d.\n", count, ifile->size);
    return 0;
}


int fs_create(fs_t* fs, inodeid_t dir, char* file, inodeid_t* fileid)
{
   if (fs == NULL || dir >= ITAB_SIZE || file == NULL || fileid == NULL) {
      printf("[fs_create] malformed arguments.\n");
      return -1;
   }

   if (strlen(file) == 0 || strlen(file)+1 > FS_MAX_FNAME_SZ){
      dprintf("[fs_create] file name size error.\n");
      return -1;
   }

   if (!BMAP_ISSET(fs->inode_bmap,dir)) {
      dprintf("[fs_create] inode is not being used.\n");
      return -1;
   }

   fs_inode_t* idir = &fs->inode_tab[dir];
   if (idir->type != FS_DIR) {
      dprintf("[fs_create] inode is not a directory.\n");
      return -1;
   }

   if (fsi_dir_search(fs,dir,file,fileid) == 0) {
      dprintf("[fs_create] file already exists.\n");
      return -1;
   }
   
   // check if there are free inodes
   unsigned finode;
   if (!fsi_bmap_find_free(fs->inode_bmap,ITAB_SIZE,&finode)) {
      dprintf("[fs_create] there are no free inodes.\n");
      return -1;
   }

   // add a new block to the directory if necessary
   if (idir->size % BLOCK_SIZE == 0) {
      unsigned fblock;
      if (!fsi_bmap_find_free(fs->blk_bmap,block_num_blocks(fs->blocks),&fblock)) {
         dprintf("[fs_create] no free blocks to augment directory.\n");
         return -1;
      }
      BMAP_SET(fs->blk_bmap,fblock);
      idir->blocks[idir->size / BLOCK_SIZE] = fblock;
   }

   // add the entry to the directory
   fs_dentry_t page[DIR_PAGE_ENTRIES];
   block_read(fs->blocks,idir->blocks[idir->size/BLOCK_SIZE],(char*)page);
   fs_dentry_t* entry = &page[idir->size % BLOCK_SIZE / sizeof(fs_dentry_t)];
   strcpy(entry->name,file);
   entry->inodeid = finode;
   block_write(fs->blocks,idir->blocks[idir->size/BLOCK_SIZE],(char*)page);
   idir->size += sizeof(fs_dentry_t);

   // reserve and init the new file inode
   BMAP_SET(fs->inode_bmap,finode);
   fsi_inode_init(&fs->inode_tab[finode],FS_FILE);

   // save the file system metadata
   fsi_store_fsdata(fs);

   *fileid = finode;
   return 0;
}


int fs_mkdir(fs_t* fs, inodeid_t dir, char* newdir, inodeid_t* newdirid)
{
	if (fs==NULL || dir>=ITAB_SIZE || newdir==NULL || newdirid==NULL) {
		printf("[fs_mkdir] malformed arguments.\n");
		return -1;
	}

	if (strlen(newdir) == 0 || strlen(newdir)+1 > FS_MAX_FNAME_SZ){
		dprintf("[fs_mkdir] directory size error.\n");
		return -1;
	}

	if (!BMAP_ISSET(fs->inode_bmap,dir)) {
		dprintf("[fs_mkdir] inode is not being used.\n");
		return -1;
	}

	fs_inode_t* idir = &fs->inode_tab[dir];
	if (idir->type != FS_DIR) {
		dprintf("[fs_mkdir] inode is not a directory.\n");
		return -1;
	}

	if (fsi_dir_search(fs,dir,newdir,newdirid) == 0) {
		dprintf("[fs_mkdir] directory already exists.\n");
		return -1;
	}
   
   	// check if there are free inodes
	unsigned finode;
	if (!fsi_bmap_find_free(fs->inode_bmap,ITAB_SIZE,&finode)) {
		dprintf("[fs_mkdir] there are no free inodes.\n");
		return -1;
	}

   	// add a new block to the directory if necessary
	if (idir->size % BLOCK_SIZE == 0) {
		unsigned fblock;
		if (!fsi_bmap_find_free(fs->blk_bmap,block_num_blocks(fs->blocks),&fblock)) {
			dprintf("[fs_mkdir] no free blocks to augment directory.\n");
			return -1;
		}
		BMAP_SET(fs->blk_bmap,fblock);
		idir->blocks[idir->size / BLOCK_SIZE] = fblock;
	}

   	// add the entry to the directory
	fs_dentry_t page[DIR_PAGE_ENTRIES];
	block_read(fs->blocks,idir->blocks[idir->size/BLOCK_SIZE],(char*)page);
	fs_dentry_t* entry = &page[idir->size % BLOCK_SIZE / sizeof(fs_dentry_t)];
	strcpy(entry->name,newdir);
	entry->inodeid = finode;
	block_write(fs->blocks,idir->blocks[idir->size/BLOCK_SIZE],(char*)page);
	idir->size += sizeof(fs_dentry_t);

   	// reserve and init the new file inode
	BMAP_SET(fs->inode_bmap,finode);
	fsi_inode_init(&fs->inode_tab[finode],FS_DIR);

   	// save the file system metadata
	fsi_store_fsdata(fs);

	*newdirid = finode;
	return 0;
}


int fs_readdir(fs_t* fs, inodeid_t dir, fs_file_name_t* entries, int maxentries,
   int* numentries)
{
    if (fs == NULL || dir >= ITAB_SIZE || entries == NULL ||
        numentries == NULL || maxentries < 0) {
        dprintf("[fs_readdir] malformed arguments.\n");
        return -1;
    }

    pthread_mutex_lock(&fs->cache_mutex);
    
    // 1. Verificar cache de inodes primeiro
    fs_inode_t* idir = NULL;
    inode_cache_entry_t* cached_inode = find_inode_in_cache(fs, dir);
    
    if (cached_inode) {
        idir = &cached_inode->inode;
        cached_inode->last_access = time(NULL); // Atualiza LRU
    } else {
        if (!BMAP_ISSET(fs->inode_bmap, dir)) {
            pthread_mutex_unlock(&fs->cache_mutex);
            dprintf("[fs_readdir] inode is not being used.\n");
            return -1;
        }
        idir = &fs->inode_tab[dir];
        add_inode_to_cache(fs, dir, idir, 0); // Adiciona à cache
    }

    if (idir->type != FS_DIR) {
        pthread_mutex_unlock(&fs->cache_mutex);
        dprintf("[fs_readdir] inode is not a directory.\n");
        return -1;
    }

    // 2. Preencher as entradas com o conteúdo do diretório
    int num = MIN(idir->size / sizeof(fs_dentry_t), maxentries);
    int iblock = 0, ientry = 0;
    
    while (num > 0) {
        unsigned int block_num = idir->blocks[iblock];
        fs_dentry_t page[DIR_PAGE_ENTRIES];
        int use_cache = 0;
        
        // 3. Verificar se o bloco do diretório está em cache
        dir_cache_entry_t* cached_dir_block = NULL;
        for (int i = 0; i < DIR_CACHE_SIZE; i++) {
            if (fs->dir_cache[i].dir_num == dir && 
                fs->dir_cache[i].block_num == block_num) {
                cached_dir_block = &fs->dir_cache[i];
                cached_dir_block->last_access = time(NULL);
                memcpy(page, cached_dir_block->entries, sizeof(page));
                use_cache = 1;
                break;
            }
        }
        
        if (!use_cache) {
            // Se não está em cache, ler do disco
            pthread_mutex_unlock(&fs->cache_mutex);
            if (block_read(fs->blocks, block_num, (char*)page)) {
                dprintf("[fs_readdir] error reading block %d\n", block_num);
                return -1;
            }
            pthread_mutex_lock(&fs->cache_mutex);
            
            // Adicionar à cache (substituição LRU)
            int lru_index = 0;
            time_t lru_time = fs->dir_cache[0].last_access;
            
            for (int i = 1; i < DIR_CACHE_SIZE; i++) {
                if (fs->dir_cache[i].last_access < lru_time) {
                    lru_index = i;
                    lru_time = fs->dir_cache[i].last_access;
                }
            }
            
            // Atualizar cache
            fs->dir_cache[lru_index].dir_num = dir;
            fs->dir_cache[lru_index].block_num = block_num;
            memcpy(fs->dir_cache[lru_index].entries, page, sizeof(page));
            fs->dir_cache[lru_index].last_access = time(NULL);
        }
        
        // 4. Processar as entradas do bloco atual
        for (int i = 0; i < DIR_PAGE_ENTRIES && num > 0; i++, num--) {
            strcpy(entries[ientry].name, page[i].name);
            
            // Verificar cache de inodes para o tipo
            inode_cache_entry_t* entry_inode = find_inode_in_cache(fs, page[i].inodeid);
            if (entry_inode) {
                entries[ientry].type = entry_inode->inode.type;
                entry_inode->last_access = time(NULL);
            } else {
                // Se não está em cache, verificar tabela principal
                if (page[i].inodeid < ITAB_SIZE && BMAP_ISSET(fs->inode_bmap, page[i].inodeid)) {
                    entries[ientry].type = fs->inode_tab[page[i].inodeid].type;
                } else {
                    entries[ientry].type = FS_UNKNOWN;
                }
            }
            
            ientry++;
        }
        
        iblock++;
    }
    
    pthread_mutex_unlock(&fs->cache_mutex);
    *numentries = ientry;
    return 0;
}



int fs_copy(fs_t* fs, char* srcpath, char* tgtpath) {
    if (fs == NULL || srcpath == NULL || tgtpath == NULL) {
        dprintf("[fs_copy] malformed arguments.\n");
        return -1;
    }

    // 1. Localizar o inode do arquivo de origem
    inodeid_t src_inode;
    if (fs_lookup(fs, srcpath, &src_inode) <= 0) {
        dprintf("[fs_copy] source file not found.\n");
        return -1;
    }

    // Verificar se é um arquivo regular
    fs_file_attrs_t src_attrs;
    if (fs_get_attrs(fs, src_inode, &src_attrs) < 0 || src_attrs.type != FS_FILE) {
        dprintf("[fs_copy] source is not a regular file.\n");
        return -1;
    }

    // 2. Extrair diretório e nome do arquivo destino
    char* last_slash = strrchr(tgtpath, '/');
    if (last_slash == NULL) {
        dprintf("[fs_copy] invalid target path.\n");
        return -1;
    }

    char target_dir[FS_MAX_FNAME_SZ];
    char target_name[FS_MAX_FNAME_SZ];
    strncpy(target_dir, tgtpath, last_slash - tgtpath);
    target_dir[last_slash - tgtpath] = '\0';
    strcpy(target_name, last_slash + 1);

    // 3. Localizar o diretório de destino
    inodeid_t dir_inode;
    if (fs_lookup(fs, target_dir, &dir_inode) <= 0) {
        dprintf("[fs_copy] target directory not found.\n");
        return -1;
    }

    // 4. Criar novo arquivo no diretório de destino
    inodeid_t new_inode;
    if (fs_create(fs, dir_inode, target_name, &new_inode) < 0) {
        dprintf("[fs_copy] failed to create target file.\n");
        return -1;
    }

    pthread_mutex_lock(&fs->cache_mutex);

    // 5. Obter inodes (usando cache)
    fs_inode_t* src_ifile = NULL;
    fs_inode_t* new_ifile = NULL;
    
    // Obter inode de origem (da cache ou tabela principal)
    inode_cache_entry_t* cached_src = find_inode_in_cache(fs, src_inode);
    if (cached_src) {
        src_ifile = &cached_src->inode;
        cached_src->last_access = time(NULL);
    } else {
        src_ifile = &fs->inode_tab[src_inode];
        add_inode_to_cache(fs, src_inode, src_ifile, 0);
    }

    // Obter inode de destino (da cache ou tabela principal)
    inode_cache_entry_t* cached_new = find_inode_in_cache(fs, new_inode);
    if (cached_new) {
        new_ifile = &cached_new->inode;
        cached_new->last_access = time(NULL);
    } else {
        new_ifile = &fs->inode_tab[new_inode];
        add_inode_to_cache(fs, new_inode, new_ifile, 1); // Marcar como dirty
    }

    // 6. Copiar os blocos (usando cache)
    int blks_used = OFFSET_TO_BLOCKS(src_ifile->size);
    for (int i = 0; i < blks_used; i++) {
        unsigned int src_block = src_ifile->blocks[i];
        unsigned int new_block;
        
        // Alocar novo bloco
        if (!fsi_bmap_find_free(fs->blk_bmap, block_num_blocks(fs->blocks), &new_block)) {
            pthread_mutex_unlock(&fs->cache_mutex);
            dprintf("[fs_copy] no free blocks available.\n");
            return -1;
        }
        BMAP_SET(fs->blk_bmap, new_block);
        new_ifile->blocks[i] = new_block;

        // Copiar dados do bloco
        char block_data[BLOCK_SIZE];
        
        // Tentar obter bloco de origem da cache
        block_cache_entry_t* cached_block = find_block_in_cache(fs, src_block);
        if (cached_block) {
            memcpy(block_data, cached_block->data, BLOCK_SIZE);
            cached_block->last_access = time(NULL);
        } else {
            // Se não está na cache, ler do disco
            pthread_mutex_unlock(&fs->cache_mutex);
            if (block_read(fs->blocks, src_block, block_data)) {
                dprintf("[fs_copy] error reading source block %d\n", src_block);
                return -1;
            }
            pthread_mutex_lock(&fs->cache_mutex);
            
            // Adicionar bloco de origem à cache
            add_block_to_cache(fs, src_block, block_data, 0);
        }

        // Adicionar novo bloco à cache (já marcando como dirty)
        add_block_to_cache(fs, new_block, block_data, 1);
    }

    // 7. Atualizar metadados do novo arquivo
    new_ifile->size = src_ifile->size;
    new_ifile->type = FS_FILE;

    // Se o inode de destino não estava na cache, atualizar tabela principal
    if (!cached_new) {
        fs->inode_tab[new_inode] = *new_ifile;
    }

    // 8. Atualizar metadados no disco (apenas se necessário)
    if (cached_new && cached_new->dirty) {
        pthread_mutex_unlock(&fs->cache_mutex);
        fsi_store_fsdata(fs);
        pthread_mutex_lock(&fs->cache_mutex);
    }

    pthread_mutex_unlock(&fs->cache_mutex);
    return 0;
}

void fs_dump(fs_t* fs)
{
   printf("Free block bitmap:\n");
   fsi_dump_bmap(fs->blk_bmap,BLOCK_SIZE);
   printf("\n");
   
   printf("Free inode table bitmap:\n");
   fsi_dump_bmap(fs->inode_bmap,BLOCK_SIZE);
   printf("\n");
}


