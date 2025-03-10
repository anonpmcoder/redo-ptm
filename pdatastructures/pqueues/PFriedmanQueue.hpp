#ifndef _PERSISTENT_FRIEDMAN_QUEUE_HP_H_
#define _PERSISTENT_FRIEDMAN_QUEUE_HP_H_

#include <atomic>
#include <stdexcept>
#include "common/pfences.h"
#include "HazardPointers.hpp"

//
// DO NOT USE THIS CLASS!!!
// This class is meant only for benchmarks to have a baseline comparison.
// It does not do correct recovery or allocation/de-allocation.
//


#include "Utilities.h"
#include "p_utils.h"

// Size of the persistent memory region
#ifndef PM_REGION_SIZE
#define PM_REGION_SIZE (2*1024*1024*1024ULL) // 2GB by default (to run on laptop)
#endif
// Name of persistent file mapping
#ifndef PM_FILE_NAME
#define PM_FILE_NAME   "/dev/shm/pmdk_shared"
#endif


#ifdef USE_PMDK_ALLOC
#include <libpmemobj++/p.hpp>
#include <libpmemobj++/transaction.hpp>
#include <libpmemobj++/pool.hpp>
#include <libpmemobj++/allocator.hpp>
#include <mutex>
using namespace pmem::obj;
auto gpopf = pool_base::create(PM_FILE_NAME, "", (size_t)(PM_REGION_SIZE));
std::mutex glockf {};
#endif

// If you see an error like this:
// terminate called after throwing an instance of 'pmem::transaction_error'
//  what():  transaction aborted
// It's because the pmdk pool is out of memory (this queue doesn't do deletion)


/**
 * <h1> Persistent lock-free Queue </h1>
 *
 * WARNING: this doesn't do memory reclamation, which means that when we enabled PMDK as the allocator,
 * it blows away all the memory quickly (just 2 threads is enough).
 * I'm not sure how to do proper memory reclamation with this, seen as returnedValues still holds pointers to the items.
 * I guess the only way to do it is to have pointers to copy of the items, and delete/retire the old ones before overwritting.
 * This in turn creates more pressure on the memory allocator... ohh well.
 *
 * This is the lock-free queue shown by Michal Friedman, Maurice Herlihy, Virendra Marathe, Erez Petrank.
 * https://dl.acm.org/citation.cfm?id=3178490
 *
 * Consistency: Durable Linearizable
 * enqueue() progress: lock-free
 * dequeue() progress: lock-free
 * Memory Reclamation: Hazard Pointers (lock-free)
 *
 * To understand what the PWB/PFENCE/PSYNC are, take a look at
 * "Preserving Happens-Before in persistent memory":
 * https://www.cs.rochester.edu/u/jhi1/papers/2016-spaa-transform
 *
 * <p>
 * The paper on Hazard Pointers is named "Hazard Pointers: Safe Memory
 * Reclamation for Lock-Free objects" and it is available here:
 * http://web.cecs.pdx.edu/~walpole/class/cs510/papers/11.pdf
 *
 */
template<typename T>
class PFriedmanQueue {

private:
    static const int FHMP_MAX_THREADS = 128;

    template<typename TN> static void internalDelete(TN* obj) {
#ifdef USE_PMDK_ALLOC
        if (obj == nullptr) return;
        obj->~TN();
        glockf.lock();
        transaction::run(gpopf, [obj] () {
            pmemobj_tx_free(pmemobj_oid(obj));
        });
        glockf.unlock();
#else
        delete obj;
#endif
    }

    template<typename TN, typename... Args> static TN* internalNew(Args&&... args) {
#ifdef USE_PMDK_ALLOC
        glockf.lock();
#ifdef MEASURE_PWB
        tl_num_pwbs += 3;
        tl_num_pfences += 2;
#endif
        void *addr = nullptr;
        transaction::run(gpopf, [&addr] () {
            auto oid = pmemobj_tx_alloc(sizeof(TN), 0);
            addr = pmemobj_direct(oid);
        });
        glockf.unlock();
        return new (addr) TN(std::forward<Args>(args)...); // placement new
#else
        return new TN(std::forward<Args>(args)...);
#endif
    }

    struct Node {
        T                   value;
        std::atomic<Node*>  next {nullptr};
        std::atomic<int>    deqThreadID {-1};
        Node(T item) : value{item} { }
        bool casNext(Node *cmp, Node *val) {
            return next.compare_exchange_strong(cmp, val);
        }
        bool casDeqTID(int cmp, int val) {
            return deqThreadID.compare_exchange_strong(cmp, val);
        }
    };

    bool casTail(Node *cmp, Node *val) {
		return tail.compare_exchange_strong(cmp, val);
	}

    bool casHead(Node *cmp, Node *val) {
        return head.compare_exchange_strong(cmp, val);
    }

    //
    // Persistent variables
    //

    // Pointers to head and tail of the list
    alignas(128) std::atomic<Node*> head {nullptr};
    alignas(128) std::atomic<Node*> tail {nullptr};
    alignas(128) std::atomic<T*> returnedValues[FHMP_MAX_THREADS];


    // Set to true when the constructor completed sucessfully
    bool constructorInProgress = true;
    // Will be set to true when the destructor is called, in case there is a crash during destructor
    bool destructorInProgress = false;
    const int maxThreads;

    std::function<void(Node*,int)> mydeleter = [](Node* node, int tid){ internalDelete(node); };

    // We need two hazard pointers for dequeue()
    // This variable is a non-volatile pointer to a volatile object
    HazardPointers<Node>* hp  = new HazardPointers<Node>{2, maxThreads,mydeleter};
    static const int kHpTail = 0;
    static const int kHpHead = 0;
    static const int kHpNext = 1;


    /*
     * To be called when restarting after a failure
     * We tried to follow the description in section 5.3 of the paper as much as possible
     */
    void recover() {
        // TODO: not yet implemented...
        if (destructorInProgress) {
            if (head.load(std::memory_order_relaxed) != nullptr) {
                while (dequeue(0) != EMPTY); // Drain the queue
                head.store(nullptr, std::memory_order_relaxed);
                PWB(&head);
                PFENCE();
                internalDelete(head.load(std::memory_order_relaxed));  // Delete the last node
            }
            PSYNC();
            return;
        }
        hp = new HazardPointers<Node>{2, maxThreads,mydeleter};

        // TODO: place recovery of head and recovery of tail here...


        // If both head is null then a failure occurred during constructor
        if (head.load(std::memory_order_relaxed) == nullptr) {
            Node* sentinelNode = internalNew<Node>(T{});
            head.store(sentinelNode, std::memory_order_relaxed);
            PWB(&head);
            PFENCE();
        }
        // If tail is null, then fix it by setting it to head
        if (tail.load(std::memory_order_relaxed) == nullptr) {
            tail.store(head.load(std::memory_order_relaxed), std::memory_order_relaxed);
            PWB(&tail);
            PFENCE();
        }
        // Advance the tail if needed
        Node* ltail = tail.load(std::memory_order_relaxed);
        Node* lnext = ltail->next.load(std::memory_order_relaxed);
        if (lnext != nullptr) {
            tail.store(lnext, std::memory_order_relaxed);
            PWB(&tail);
        }
        PSYNC();
    }


public:
    T EMPTY {};

    // This is "DurableQueue()" of Figure 1 of the paper.
    // Unfortunately, this code is incorrect without some kind of "validation" flag, so we added it
    PFriedmanQueue(int maxThreads=FHMP_MAX_THREADS) : maxThreads{maxThreads} {
        if (!constructorInProgress) return;
        Node* node = internalNew<Node>(T{});
        PWB(node); // We're assuming this flushes the whole node
        PFENCE();
        head = node;
        PWB(&head);
        PFENCE();
        tail = node;
        PWB(&tail);
        PFENCE();
        for (int i = 0; i < maxThreads; i++) {
            returnedValues[i].store(nullptr, std::memory_order_release);
            PWB(&returnedValues[i]);
            PFENCE();
        }
        constructorInProgress = false;
        PWB(&constructorInProgress);
        PFENCE();
    }

    // There is no destructor in the original code, therefore we had to make one
    ~PFriedmanQueue() {
        destructorInProgress = true;
        PWB(&destructorInProgress);
        PFENCE();
        recover();  // Re-using the same code from the recovery method
    }

    static std::string className() { return "PFriedmanQueue"; }

    /*
     * Code taken from enq() in figure 2 of the paper.
     * Progress: lock-free
     * Uncontended: 2 PWB, 2 PFENCE, 2 CAS,
     */
    void enqueue(T item, const int tid) {
        Node* node = internalNew<Node>(item);
        PWB(&node->value); // We flush multiple variables, just in case they are not on the same cache line
        PWB(&node->next);
        PFENCE();             // This isn't really needed, but it's in the paper so we leave it
        while (true) {
            Node* last = hp->protectPtr(kHpTail, tail, tid);
            if (last == tail.load()) {
                Node* next = last->next.load();
                if (next == nullptr) {
                    if (last->casNext(nullptr, node)) {
                        PWB(&last->next);
                        PSYNC(); // This isn't really needed because of the following CAS, but it's in the paper
                        casTail(last, node);
                        hp->clear(tid);
                        return;
                    }
                } else {
                    PWB(&last->next);
                    PSYNC(); // This isn't really needed because of the following CAS, but it's in the paper
                    casTail(last, next);
                }
            }
        }
    }

    /*
     * Code taken from deq() in figure 3 of the paper.
     * Progress: lock-free
     * Uncontended: 4 PWB, 4 PFENCE, 2 CAS, 1 MFENCE (for the seq-cst store in returnedValues[tid])
     */
    T dequeue(const int tid) {
        T* newReturnedValue = internalNew<T>();
        PWB(newReturnedValue); // Flush the contents of T. We're assuming T is on the same cache line
        PFENCE();
        returnedValues[tid] = newReturnedValue;
        PWB(&returnedValues[tid]);
        PFENCE();
        while (true) {
            Node* first = hp->protectPtr(kHpHead, head, tid);
            Node* last = tail;
            if (first == head) {
                Node* next = first->next.load();
                if (first == last) {
                    if (next == nullptr) {
                        *returnedValues[tid] = EMPTY;
                        PWB(returnedValues[tid].load());
                        PSYNC();
                        hp->clear(tid);
                        return EMPTY;
                    }
                    PWB(&last->next);
                    PFENCE();
                    casTail(last, next);
                } else {
                    T value = next->value;
                    if (next->casDeqTID(-1, tid)) {
                        PWB(&(first->next.load()->deqThreadID));
                        PFENCE();
                        *returnedValues[tid] = value;
                        PWB(returnedValues[tid].load());
                        PSYNC();
                        if (casHead(first, next));// hp->retire(first, tid);
                        hp->clear(tid);
                        return *returnedValues[tid];
                    } else {
                        T* address = returnedValues[next->deqThreadID];
                        if (head == first) { //same context
                            PWB(&(first->next.load()->deqThreadID));
                            PFENCE();
                            *address = value;
                            PWB(address);
                            PFENCE();
                            if (casHead(first, next));// hp->retire(first, tid);
                        }
                    }
                }
            }
        }
    }
};

#endif /* _PERSISTENT_FRIEDMAN_QUEUE_HP_H_ */
