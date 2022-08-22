#include <unistd.h>
#include <iostream>
#include <cstring>
#include <sys/mman.h>

#define INITIAL_MMAP_THREASHOLD 128*1024
#define HUGE_SCALLOC 1024*1024*2
#define HUGE_SMALLOC 1024*1024*4

size_t align (size_t size);
void* Sbrk(size_t size)
{
    void* p = sbrk(0);
    if (p == (void*)(-1))
    {
        return nullptr;
    }
    unsigned long base = (unsigned long)p;
    if(base % 8 != 0)
    {
        p = sbrk(8 - base % 8);
        if (p == (void*)(-1))
        {
            return nullptr;
        }
    }
    p = sbrk(size);
    if (p == (void*)(-1))
    {
        return nullptr;
    }
    return p;
}
typedef struct  malloc_meta_data_t{
    size_t size;
    bool is_free;
    void* p;
    bool is_mmap;
    bool is_scalloc;
    malloc_meta_data_t* lower;
    malloc_meta_data_t* higher;
    malloc_meta_data_t* free_next;
    malloc_meta_data_t* free_prev;
}MallocMetadata;

class MallocList {
    size_t free_blocks;
    size_t alloc_blocks; //free & used
    size_t free_bytes;
    size_t alloc_bytes; //free & used
    MallocMetadata* free_list_head;//  free list
    MallocMetadata* mmaped_list_head;
    MallocMetadata* wilderness;// end of all blocks list
    
    size_t mmap_threshold;

public:
    MallocList()
    {
        this->free_blocks = 0;
        this->alloc_blocks = 0;
        this->free_bytes = 0;
        this->alloc_bytes = 0;
        this->free_list_head = nullptr;
        this->wilderness = nullptr;
        this->mmaped_list_head = nullptr;
        this->mmap_threshold = INITIAL_MMAP_THREASHOLD;
    }
    size_t getMmapThreshold()
    {
        return this->mmap_threshold;
    }
    void updateBusyBlock(MallocMetadata* md)
    {
        if (md != nullptr)
        {
            md->is_free = false;
            md->free_next = nullptr;
            md->free_prev = nullptr;
            md->is_mmap = false;
        }
    }
    //update all pointers to null and is_free <- false
    void updateNewBlock(MallocMetadata* md)
    {
        if (md != nullptr) 
        {
            md->is_free = false;
            md->lower = nullptr;
            md->higher = nullptr;
            md->free_next = nullptr;
            md->free_prev = nullptr;
            md->is_mmap = false;
        }
    }
    static MallocList& getInstance() // make MallocList singleton
    {
        static MallocList instance; // Guaranteed to be destroyed.
        // Instantiated on first use.
        return instance;
    }
    MallocMetadata* split(MallocMetadata* old_md, size_t size)
    {       
        MallocMetadata* new_free_md = (MallocMetadata*)((char*)old_md->p + size);
        new_free_md->size = old_md->size - size - sizeof(MallocMetadata);
        new_free_md->lower = old_md;
        new_free_md->higher = old_md->higher;
        new_free_md->p = (char*)old_md->p + size + sizeof(MallocMetadata);
        new_free_md->is_mmap = false;
        if (old_md->higher != nullptr)
        {
            old_md->higher->lower = new_free_md;
        }
        else
        {
            this->wilderness = new_free_md;
        }
        old_md->higher = new_free_md;
        old_md->is_free = false;
        old_md->size = size;
        this->alloc_blocks++;
        this->alloc_bytes -= sizeof(MallocMetadata);
        this->freeBlock(new_free_md->p); //inserting new free block to free list
        
        return old_md; //newly allocated block
    }
    MallocMetadata* mergeAdjBlocks (MallocMetadata* low, MallocMetadata* high, bool is_free)
    {
        this->free_blocks--;
        this->alloc_blocks--;
        this->alloc_bytes += sizeof(MallocMetadata);
        low->size += high->size + sizeof(MallocMetadata);
        low->higher = high->higher;
        if(high->higher != nullptr)
        {
            (high->higher)->lower = low;
        }
        if (this->wilderness == high)
        {
            this->wilderness = low;
        }
        if (!is_free)
        {   
            this->free_bytes -= (low->size - sizeof(MallocMetadata));
            if(low->free_prev != nullptr)
            {
                low->free_prev->free_next = low->free_next;
            }
            if(low->free_next != nullptr)
            {
                low->free_next->free_prev = low->free_prev;
            }
            this->updateBusyBlock(low);
            return low;
        }
        //is_free:
        if(high == this->free_list_head)
        {
            this->free_list_head = low;
        }
        this->free_bytes += sizeof(MallocMetadata);
        if (this->alloc_blocks == 1)
        {
            low->free_next = nullptr;
            low->free_prev = nullptr;
            return low;
        }
        if (low->free_next == nullptr && low->free_prev == nullptr)
        {
            low->free_next = high->free_next;
            low->free_prev = high->free_prev;
            if (high->free_prev != nullptr)
            {
                (high->free_prev)->free_next = low;
            }
            if (high->free_next != nullptr)
            {
                (high->free_next)->free_prev = low;
            }
            return low;
        }
        if (low->free_next == nullptr )
        {
            return low;
        }
        if (low->free_next->size < low->size || 
        (low->free_next->size == low->size && low->free_next->p < low->p))
        {
            (low->free_next)->free_prev = low->free_prev;
            if (low->free_prev != nullptr)
            {
                (low->free_prev)->free_next = low->free_next;
            }
            MallocMetadata* tmp = low->free_next;
            while(tmp->free_next != nullptr && tmp->size < low->size)
            {
                tmp = tmp->free_next;
            }
            if ((tmp->size > low->size) || (tmp->size == low->size && tmp->p < low->p))
            {
                tmp = tmp->free_prev;
            }
            //tmp ---> low ---> (tmp->free_next)
            if (tmp->free_next != nullptr)
            {
                tmp->free_next->free_prev = tmp;
            }
            low->free_next = tmp->free_next;
            tmp->free_next = low;
            low->free_prev = tmp;
        }
        return low;
    }
    MallocMetadata* unionWilderness(size_t size)
    {
        size_t new_space = size - this->wilderness->size;
        void* p = Sbrk(new_space);
        if (p == nullptr)
        {
            return nullptr;
        }
        this->wilderness->size = size;
        this->alloc_bytes += new_space;
        return this->wilderness;
    }
    void insertBigBlock (MallocMetadata* meta_data)
    {
        if (meta_data == nullptr)
        {
            return;
        }
        meta_data->is_mmap = true;
        this->alloc_blocks ++;
        this->alloc_bytes += meta_data->size;
        MallocMetadata* curr = this->mmaped_list_head;
        MallocMetadata* prev = nullptr;
        while (curr != nullptr && curr->p < meta_data->p)
        {
            prev = curr;
            curr = curr->higher;
        }
        meta_data->higher = curr;
        meta_data->lower = prev;
        if (prev == nullptr)
        {
            this->mmaped_list_head = meta_data;
        }
        else
        {
            prev->higher = meta_data;
        }
        if (curr != nullptr)
        {
            curr->lower = meta_data;
        }
    }
    // lower and  higher = null
    // free_next and free_prev = null, already allocated , is_free = false, not mmapped
    void insertNewAllocatedBlock (MallocMetadata* meta_data) 
    {
        if (meta_data == nullptr)
        {
            return;
        }
        if (this->wilderness == nullptr)
        {
            this->wilderness = meta_data;
        }
        else
        {
            this->wilderness->higher = meta_data;
            meta_data->lower = this->wilderness;
            this->wilderness = meta_data;
        }
        this->alloc_blocks ++;
        this->alloc_bytes += meta_data->size;
    }
    MallocMetadata* reallocateBigBlock (MallocMetadata* md, size_t size)
    {
        if (md == nullptr)
        {
            return nullptr;
        }
        if (md->size == size)
        {
            return md;
        }
        size_t move_size = (size < md->size) ? size : md->size;
        bool is_scalloc = (md == nullptr) ? false : md->is_scalloc;
        MallocMetadata* new_md = this->allocateBigBlock(size, is_scalloc);
        if (new_md != nullptr)
        {
            memmove(new_md->p, md->p, move_size);    
            //after success we free oldp
            this->freeBigBlock(md);
        }
        return new_md;
    }
    MallocMetadata* reallocateBlock (MallocMetadata* md, size_t size)
    {
        if (md == nullptr)
        {
            return nullptr;
        }
        if (md->size >= size)
        {
            if (md->size >= 128 + sizeof(MallocMetadata) + size)
            {
                return split(md, size);
            }
            return md;
        }
        size_t oldsize = md->size;
        void* oldp = md->p;
        MallocMetadata* merged = md;
        bool merge_with_lower = md->lower != nullptr && md->lower->is_free;
        bool merge_with_higher = (md->higher != nullptr && md->higher->is_free) && (md->size + md->higher->size >= size);
        if (merge_with_lower && ((md->size + md->lower->size >= size) || !merge_with_higher)) //mrege with lower
        {
            if (this->free_list_head == md->lower)
            {
                this->free_list_head = md->lower->free_next;
            }
            this->free_bytes += md->size;
            merged = this->mergeAdjBlocks(md->lower, md, false);
            if (merged->size >= size)
            {
                return copyAndSplit(merged, size, oldp, oldsize);
            }
        }
        if (merged->higher != nullptr && merged->higher->is_free) //merge with higher
        {
            if (this->free_list_head == merged->higher)
            {
                this->free_list_head = merged->higher->free_next;
            }
            this->free_bytes += merged->size;
            merged = this->mergeAdjBlocks(merged, merged->higher, false);
        }
        if (merged->size >= size)
        {
            return copyAndSplit (merged, size, oldp, oldsize);
        }
        if (merged->higher == nullptr) //enlarge wilderness
        {
            this->unionWilderness(size);
        }
        if (merged->size >= size)
        {
            return copyAndSplit(merged, size, oldp, oldsize);
        }
        else //find other block
        {
            MallocMetadata* new_md = this->findFreeBlock(size);
            memmove(new_md->p, oldp, oldsize);    
            //after success we free oldp
            this->freeBlock(merged->p);
            return new_md;
        }
    }
    MallocMetadata* copyAndSplit (MallocMetadata* md, size_t size, void* oldp, size_t oldsize)
    {
        if (oldp != md->p)
        {
            memmove(md->p, oldp, oldsize);
        }
        if (md->size >= 128 + sizeof(MallocMetadata) + size)
        {
            return split(md, size);
        }
        return md;
    }
    
    MallocMetadata* allocateBigBlock(size_t size, bool is_scalloc)
    {
        int flags = MAP_ANONYMOUS | MAP_PRIVATE;
        if (size >= HUGE_SMALLOC || (size >= HUGE_SCALLOC && is_scalloc))
        {
            flags = flags | MAP_HUGETLB;
        }
        void* p = mmap(nullptr, size + sizeof(MallocMetadata), PROT_READ | PROT_WRITE, flags, -1, 0);
        if (p == (void*)(-1))
        {
            std::cout<<"HELLO";
            return nullptr;
        }
        MallocMetadata* new_md = (MallocMetadata*)p;
        new_md->size = size;
        new_md->p = (void*)((MallocMetadata*)p + 1);
        this->updateNewBlock(new_md);
        this->insertBigBlock(new_md); //inside new_md->is_mmap = true
        new_md->is_scalloc = is_scalloc;
        return new_md;
    }

    MallocMetadata* findFreeBlock (size_t size)
    {
        MallocMetadata* tmp = this->free_list_head;
        while (tmp != nullptr && tmp->size < size )
        {
            tmp = tmp->free_next;
        }
        if (tmp == nullptr)
        {
            //if there is no other free block return (if free) wilderness that is smaller than size
            if (this->wilderness != nullptr && this->wilderness->is_free)
            {
                if (this->free_list_head == this->wilderness)
                {
                    this->free_list_head = nullptr;
                }
                size_t old_size = this->wilderness->size;
                MallocMetadata* meta_ret = unionWilderness(size);
                if (meta_ret == nullptr)
                {//sbrk failed
                    return nullptr;
                }
                this->updateBusyBlock(this->wilderness); //updates free, free next & prev
                this->free_blocks --;
                this->free_bytes -= old_size;
                return meta_ret;
            }
            else 
            {
                //allocate a new block if there is no free block available
                void* p = Sbrk(size + sizeof(MallocMetadata));
                if (p == nullptr)
                {
                    return nullptr;
                }
                MallocMetadata* meta_data = (MallocMetadata*)p;
                this->updateNewBlock(meta_data);
                meta_data->size = size;
                meta_data->p = (void*)((MallocMetadata*)p + 1);
                this->insertNewAllocatedBlock(meta_data);
                return meta_data;
            }
        }
        else
        {
            if (this->free_list_head == tmp)
            {
                this->free_list_head = tmp->free_next;
            }
            //there is a block that is big enough
            this->updateBusyBlock(tmp); //updates free, free next & prev
            this->free_blocks --;
            this->free_bytes -= tmp->size;
            if (tmp->size >= 128 +  sizeof(MallocMetadata) + size)
            {
                return split(tmp, size);
            }
            return tmp;
        }   
    }
    //get allocated block by pointer
    MallocMetadata* getBlock (void* p)
    {
        if (p == nullptr)
        {
            return nullptr;
        }
        MallocMetadata* md = (MallocMetadata*)((MallocMetadata*)p - 1);
        return md;
    }

    void freeBigBlock(MallocMetadata* tmp)
    {
        if (tmp->lower != nullptr) 
        {
            tmp->lower->higher = tmp->higher;
        }
        if (tmp->higher != nullptr)
        {
            tmp->higher->lower = tmp->lower;
        }
        if (this->mmaped_list_head == tmp)
        {
            this->mmaped_list_head = tmp->higher;
        }
        this->alloc_bytes -= tmp->size;
        this->alloc_blocks --;
        if (tmp->size > this->mmap_threshold)
        {
            this->mmap_threshold = tmp->size;
        }
        munmap(tmp, sizeof(MallocMetadata) + tmp->size);
    }

    void freeBlock (void * p)
    {
        if (p == nullptr)
        {
            return ;
        }
        MallocMetadata* md = (MallocMetadata*)((MallocMetadata*)p - 1); 
        if (md->is_mmap)
        {
            this->freeBigBlock(md);
            return;
        }
        md->is_free = true;
        this->free_blocks ++;
        this->free_bytes += md->size;
        bool is_merged = false;
        if (md->higher != nullptr && md->higher->is_free)
        {
            is_merged = true;
            this->mergeAdjBlocks(md, md->higher, true);
        }
        if (md->lower != nullptr && md->lower->is_free)
        {
            is_merged = true;
            this->mergeAdjBlocks(md->lower, md, true);
        }
        if (!is_merged)
        {
            this->insertFreeBlock(md);
        }
    }

    void insertFreeBlock(MallocMetadata* meta)
    {
        MallocMetadata* tmp = this->free_list_head;
        MallocMetadata* prev = nullptr;
        while (tmp != nullptr && tmp->size < meta->size)
        {
            prev = tmp;
            tmp = tmp->free_next;
        }
        while(tmp != nullptr && tmp->size == meta->size && tmp->p < meta->p)
        {
            prev = tmp;
            tmp = tmp->free_next;
        }
        meta->free_prev = prev;
        if (prev != nullptr)
        {
            prev->free_next = meta;
        }
        else
        {
            this->free_list_head = meta;
        }
        meta->free_next = tmp;
        if (tmp != nullptr)
        {
            tmp->free_prev = meta;
        }
    }
    size_t getAllocBytes()
    {
        return this->alloc_bytes;
    }
    size_t getFreeBytes ()
    {
        return this->free_bytes;
    }
    size_t getAllocBlocks()
    {
        return this->alloc_blocks;
    }
    size_t getFreeBlocks ()
    {
        return this->free_blocks;
    }   
};


size_t _num_free_blocks()
{
  MallocList& m_list = MallocList::getInstance();
  return m_list.getFreeBlocks();
}

size_t _num_free_bytes()
{
    MallocList& m_list = MallocList::getInstance();
    return m_list.getFreeBytes();
}

size_t _num_allocated_blocks()
{
    MallocList& m_list = MallocList::getInstance();
    return m_list.getAllocBlocks();
}

size_t _num_allocated_bytes()
{
    MallocList& m_list = MallocList::getInstance();
    return m_list.getAllocBytes();
}

size_t _size_meta_data()
{
    //Returns the number of bytes of a single meta-data structure in your system.
    return sizeof(MallocMetadata);
}

size_t _num_meta_data_bytes()
{
    MallocList& m_list = MallocList::getInstance();
    return m_list.getAllocBlocks() * _size_meta_data();
}

size_t align (size_t size)
{
    if (size % 8 == 0)
    {
        return size;
    }
    return 8*((size / 8) + 1);
}
MallocMetadata* allocateBlock(size_t size, bool is_scalloc)
{
    if (size == 0 || size > 1e8 )
    {
        return nullptr;
    }
    size = align(size);
    MallocList& m_list = MallocList::getInstance();
    if (size >= m_list.getMmapThreshold())
    {
        MallocMetadata* new_md = m_list.allocateBigBlock(size, is_scalloc);
        return new_md;
    }
    return m_list.findFreeBlock(size);
}

void* smalloc(size_t size)
{
    MallocMetadata* result = allocateBlock(size, false);
    if (result == nullptr)
    {
        return nullptr;
    }
    return result->p;
}

void* scalloc(size_t num, size_t size)
{
    MallocMetadata* result = allocateBlock(size*num, true);
    if (result == nullptr)
    {
        return nullptr;
    }
    memset(result->p, 0, size*num);
    return result->p;
}

void sfree(void* p)
{
    MallocList& m_list = MallocList::getInstance();
    m_list.freeBlock(p);
}

void* srealloc(void* oldp, size_t size)
{
    if (size == 0 || size > 1e8 )
    {
        return nullptr;
    }
    if(oldp == nullptr)
    {
        MallocMetadata* result = allocateBlock(size, false); //realloc with oldp null is malloc
        if (result == nullptr)
        {
            return nullptr;
        }
        return result->p;    
    }
    size = align(size);
    MallocList& m_list = MallocList::getInstance();
    MallocMetadata* old_meta_data = m_list.getBlock(oldp);
    MallocMetadata* result = nullptr;
    if (old_meta_data->is_mmap)
    {
        result = m_list.reallocateBigBlock(old_meta_data, size);
    }
    else
    {
        result = m_list.reallocateBlock(old_meta_data, size);
    }
    return result->p;
}
