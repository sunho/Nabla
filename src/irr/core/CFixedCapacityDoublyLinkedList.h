#ifndef __DOUBLY_LINKED_LIST_H_INCLUDED__
#define __DOUBLY_LINKED_LIST_H_INCLUDED__
#include "irr/core/alloc/PoolAddressAllocator.h"
#include "irr/core/Types.h"

namespace irr {
	namespace core {
		template<typename Value>
		struct Snode
		{
			_IRR_STATIC_INLINE_CONSTEXPR uint32_t invalid_iterator = 0xdeadbeefu;

			Value data;
			uint32_t prev;
			uint32_t next;

			Snode(const Value& val) : data(val)
			{
				prev = invalid_iterator;
				next = invalid_iterator;
			}
			Snode(Snode<Value>&& other) : data(std::move(other.data)), prev(std::move(other.prev)), next(std::move(other.next))
			{
			}

			Snode<Value>& operator=(const Snode<Value>& other)
			{
				this.data = other.data;
				this.prev = other.prev;
				this.next = other.next;
				return *this;
			}

			Snode<Value>& operator=(Snode<Value>&& other)
			{
				this.data = std::move(other.data);
				this.prev = std::move(other.prev);
				this.next = std::move(other.next);
				return *this;
			}
		};

		template<typename Value>
		class DoublyLinkedList
		{		
			_IRR_STATIC_INLINE_CONSTEXPR uint32_t invalid_iterator = 0xdeadbeefu;

			void* reservedSpace;
			PoolAddressAllocator<uint32_t> alloc;
			uint32_t m_back;
			uint32_t m_begin;
			Snode<Value>* m_array;
			uint32_t cap;


		public:

			inline void popBack()
			{
				if(m_back->prev != invalid_iterator)
					m_back->prev->next = invalid_iterator;
				uint32_t temp = m_back;
				m_back = m_back->prev;
				alloc.free_addr(temp, 1u);
			}

			inline void pushFront(Value &&val) 
			{
				uint32_t addr = alloc.alloc_addr(1u, 1u);
				Snode<Value>* n = m_array[addr];
				n->prev = invalid_iterator;
				n->data = std::move(val);
				n->next = m_begin;

				if (m_begin != invalid_iterator)
					m_begin->prev = addr;
				m_begin = addr;
			}

			inline Snode<Value>* getBegin() { return m_array[m_begin]; }

			inline Snode<Value>* getBack() { return m_array[m_back]; }

			inline void erase(uint32_t nodeAddr)
			{
				if(nodeAddr->prev != invalid_iterator)
				nodeAddr->prev->next = nodeAddr->next;
				if (nodeAddr->next != invalid_iterator)
				nodeAddr->next->prev = nodeAddr->prev;
				nodeAddr->~Snode<Value>();
				alloc.free_addr(nodeAddr, 1u);
			}

			inline void moveToFront(uint32_t nodeAddr)
			{
				if (m_begin == nodeAddr) return;
				m_begin->prev = nodeAddr;
				if (nodeAddr->next != invalid_iterator)
					nodeAddr->next->prev = nodeAddr->prev;
				if (nodeAddr->prev != invalid_iterator)
					nodeAddr->next->prev = nodeAddr->prev;
				nodeAddr->next = m_begin;
				nodeAddr->prev = invalid_iterator;
				m_begin = nodeAddr;
			}

			DoublyLinkedList(const uint32_t& capacity) :
				cap(capacity)
				reservedSpace(_IRR_ALIGNED_MALLOC(PoolAddressAllocator<uint32_t>::reserved_size(1u, capacity, 1u), alignof(void*))),
				alloc(reservedSpace, 0u, 0u, 1u, capacity, 1u),
			{
				m_back = invalid_iterator;
				m_begin = invalid_iterator;
				m_array = new Snode<Value>[capacity];
			}
			~DoublyLinkedList()
			{
				_IRR_ALIGNED_FREE(reservedSpace);
			}
		};


	}


}


#endif // !__DOUBLY_LINKED_LIST_H_INCLUDED__
