#include <unistd.h>
#include <iostream>
#include <cstring>

typedef struct  malloc_meta_data_t{
    size_t size;
    bool is_free;
    void* p;
    malloc_meta_data_t* next;
    malloc_meta_data_t* prev;
}MallocMetadata;

class MallocList {
    size_t free_blocks;
    size_t alloc_blocks; //free & used
    size_t free_bytes;
    size_t alloc_bytes; //free & used
    MallocMetadata* list_head;
public:
    MallocList()
    {
        this->free_blocks = 0;
        this->alloc_blocks = 0;
        this->free_bytes = 0;
        this->alloc_bytes = 0;
        this->list_head = nullptr;
    }

    static MallocList& getInstance() // make MallocList singleton
    {
        static MallocList instance; // Guaranteed to be destroyed.
        // Instantiated on first use.
        return instance;
    }
    void insertNewBlock (MallocMetadata* meta_data) // next and prev = null, already allocated , is_free = false
    {
        if (meta_data == nullptr)
        {
            return;
        }
        if (this->list_head == nullptr)
        {
            this->list_head = meta_data;
        }
        else
        {
            MallocMetadata* tmp = this->list_head;
            while (tmp->next != nullptr)
            {
                tmp = tmp->next;
            }
            if (tmp->p == meta_data->p)
            {
                return;
            }
            tmp->next = meta_data;
            meta_data->prev = tmp;
        }
        this->alloc_blocks ++;
        this->alloc_bytes += meta_data->size;
    }
    void* findFreeBlock (size_t size)
    {
        if (this->alloc_blocks == 0 || this->free_blocks == 0 || this->free_bytes < size)
        {
            return nullptr;
        }
        MallocMetadata* tmp = this->list_head;
        while (tmp != nullptr && (tmp->size < size || !tmp->is_free ))
        {
            tmp = tmp->next;
        }
        if (tmp == nullptr)
        {
            return nullptr;
        }
        tmp->is_free = false;
        this->free_blocks --;
        this->free_bytes -= tmp->size;
        return (void*)tmp;
    }
    MallocMetadata* getBlock (void* p)
    {
        if (p == nullptr)
        {
            return nullptr;
        }
        MallocMetadata* md = (MallocMetadata*)((uint8_t*)p - sizeof(MallocMetadata));
        return md;
    }
    void freeBlock (void * p)
    {
        if (p == nullptr)
        {
            return ;
        }
        MallocMetadata* tmp = this->list_head;
        while (tmp != nullptr && tmp->p != p)
        {
            tmp = tmp->next;
        }
        if (tmp == nullptr)
        {
            return ;
        }
        tmp->is_free = true;
        this->free_blocks ++;
        this->free_bytes += tmp->size;
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

void* allocateBlock(size_t size)
{
    if (size == 0 || size > 1e8 )
    {
        return nullptr;
    }
    MallocList& m_list = MallocList::getInstance();
    void* result = m_list.findFreeBlock(size);
    if (result != nullptr)
    {
        return result;
    }
    result = sbrk(size + sizeof(MallocMetadata));
    if (result == (void*)(-1))
    {
        return nullptr;
    }
    MallocMetadata* meta_data = (MallocMetadata*)result;
    meta_data->next = nullptr;
    meta_data->prev = nullptr;
    meta_data->is_free = false;
    meta_data->size = size;
    meta_data->p = (void*)((uint8_t*)result + sizeof(MallocMetadata));
    m_list.insertNewBlock(meta_data);
    return meta_data;
}

void* smalloc(size_t size)
{
    void* result = allocateBlock(size);
    if (result == nullptr)
    {
        return nullptr;
    }
    MallocMetadata* meta_data = (MallocMetadata*)result;
    return meta_data->p;
}

void* scalloc(size_t num, size_t size)
{
    void* result = allocateBlock(size*num);
    if (result == nullptr)
    {
        return nullptr;
    }
    MallocMetadata* meta_data = (MallocMetadata*)result;
    memset(meta_data->p, 0, meta_data->size);
    return meta_data->p;
}

void sfree(void* p)
{
    MallocList& m_list = MallocList::getInstance();
    m_list.freeBlock(p);
}

void* srealloc(void* oldp, size_t size)
{
    MallocList& m_list = MallocList::getInstance();
    MallocMetadata* old_meta_data = m_list.getBlock(oldp);
    if (old_meta_data != nullptr && old_meta_data->size >= size)
    {
        return oldp;
    }
    void* result = allocateBlock(size);
    if (result == nullptr)
    {
        return nullptr;
    }
    MallocMetadata* meta_data = (MallocMetadata*)result;
    if (old_meta_data != nullptr)
    {
        memmove(meta_data->p, oldp, old_meta_data->size);    
        //after success we free oldp
        m_list.freeBlock(old_meta_data->p);
    }
    return meta_data->p;
}
