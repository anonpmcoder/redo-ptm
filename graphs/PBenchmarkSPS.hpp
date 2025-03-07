#ifndef _PERSISTENT_BENCHMARK_SPS_H_
#define _PERSISTENT_BENCHMARK_SPS_H_

#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <typeinfo>

static const uint64_t arraySize=1000*1000;

using namespace std;
using namespace chrono;


/**
 * This is a micro-benchmark with integer swaps (SPS) for PTMs
 */
class PBenchmarkSPS {

private:
    bool array_is_init = false;

public:
    struct UserData  {
        long long seq;
        int tid;
        UserData(long long lseq, int ltid) {
            this->seq = lseq;
            this->tid = ltid;
        }
        UserData() {
            this->seq = -2;
            this->tid = -2;
        }
        UserData(const UserData &other) : seq(other.seq), tid(other.tid) { }

        bool operator < (const UserData& other) const {
            return seq < other.seq;
        }
    };

    PBenchmarkSPS() {
    }


    /*
     * An array of integers that gets randomly permutated.
     */
    template<typename PTM, template<typename> class PERSIST>
    uint64_t benchmarkSPSInteger(std::string& className, const seconds testLengthSeconds, const long numSwapsPerTx, const int numRuns, int numThreads) {
        long long ops[numThreads][numRuns];
        long long lengthSec[numRuns];
        atomic<bool> startFlag = { false };
        atomic<bool> quit = { false };


        int larraySize = arraySize;


        if(!array_is_init){
        	// Create the array of integers and initialize it, saving it in root pointer 0
            PTM::template updateTx<bool>([larraySize] () {
                //PTM::pfree( PTM::template get_object<PERSIST<uint64_t>>(0) ); // TODO: re-enable this after we add the clear of objects as a transaction in CX
                PTM::put_object(0, PTM::pmalloc( larraySize*sizeof(PERSIST<uint64_t>*) ));
                return true;
            });
            // Initialize the SPS array
			PTM::template updateTx<bool>([=] () {
				PERSIST<uint64_t>* parray = PTM::template get_object<PERSIST<uint64_t>>(0);
				for (int64_t j = 0; j < arraySize; j++) parray[j] = j;
				return true;
			});
			array_is_init = true;
        }

        auto func = [this,&startFlag,&quit,&numSwapsPerTx](long long *ops, const int tid) {
            uint64_t seed = (tid*1024)+tid+1234567890123456781ULL;
            int larraySize = arraySize;
            // Spin until the startFlag is set
            while (!startFlag.load()) {}
            // Do transactions until the quit flag is set
            long long tcount = 0;
            while (!quit.load()) {
                // Everything has to be captured by value, or get/put in root pointers
                PTM::template updateTx<bool>([seed,numSwapsPerTx,larraySize] () {
                    PERSIST<uint64_t>* parray = PTM::template get_object<PERSIST<uint64_t>>(0);
                    uint64_t lseed = seed;
                    for (int i = 0; i < numSwapsPerTx; i++) {
                        lseed = randomLong(lseed);
                        auto ia = lseed%arraySize;
                        uint64_t tmp = parray[ia];
                        lseed = randomLong(lseed);
                        auto ib = lseed%arraySize;
                        parray[ia] = parray[ib];
                        parray[ib] = tmp;
                    }
                    return true;
                });
                // Can't have capture by ref for wait-free, so replicate seed advance outside tx
                seed = randomLong(seed);
                seed = randomLong(seed);
                ++tcount;
            }
            *ops = tcount;
        };
        for (int irun = 0; irun < numRuns; irun++) {
            if (irun == 0) {
                className = PTM::className();
                cout << "##### " << PTM::className() << " #####  \n";
            }
            thread enqdeqThreads[numThreads];
            for (int tid = 0; tid < numThreads; tid++) enqdeqThreads[tid] = thread(func, &ops[tid][irun], tid);
            auto startBeats = steady_clock::now();
            startFlag.store(true);
            // Sleep for 20 seconds
            this_thread::sleep_for(testLengthSeconds);
            quit.store(true);
            auto stopBeats = steady_clock::now();
            for (int tid = 0; tid < numThreads; tid++) enqdeqThreads[tid].join();
            lengthSec[irun] = (stopBeats-startBeats).count();
            startFlag.store(false);
            quit.store(false);
        }

        // Check that the SPS array is consistent. Should work with CX and other wait-free PTMs
        PTM::template readTx<bool>([=] () {
            int64_t sum = 0;
            PERSIST<uint64_t>* parray = PTM::template get_object<PERSIST<uint64_t>>(0);
            for (int64_t j = 0; j < arraySize; j++) sum += parray[j];
            if (sum != arraySize*(arraySize-1)/2) {
                printf("ERROR in SPS validation: sum = %ld  but should be  %ld\n",  sum, arraySize*(arraySize-1)/2);
            }
            assert(sum == arraySize*(arraySize-1)/2);
            return true;
        } );
/*
        PTM::template updateTx<bool>([] () {
            PTM::pfree( PTM::template get_object<PERSIST<uint64_t>>(0) );
            PTM::template put_object<void>(0, nullptr);
            return true;
        });
*/
        // Accounting
        vector<long long> agg(numRuns);
        for (int irun = 0; irun < numRuns; irun++) {
        	for(int i=0;i<numThreads;i++){
        		agg[irun] += ops[i][irun]*1000000000LL/lengthSec[irun];
        	}
        }
        // Compute the median. numRuns should be an odd number
        sort(agg.begin(),agg.end());
        auto maxops = agg[numRuns-1];
        auto minops = agg[0];
        auto medianops = agg[numRuns/2];
        auto delta = (long)(100.*(maxops-minops) / ((double)medianops));
        // Printed value is the median of the number of ops per second that all threads were able to accomplish (on average)
        std::cout << "Swaps/sec = " << medianops*numSwapsPerTx << "     delta = " << delta*numSwapsPerTx << "%   min = " << minops*numSwapsPerTx << "   max = " << maxops*numSwapsPerTx << "\n";
        return medianops*numSwapsPerTx;
    }


    /**
     * An imprecise but fast random number generator
     */
    static uint64_t randomLong(uint64_t x) {
        x ^= x >> 12; // a
        x ^= x << 25; // b
        x ^= x >> 27; // c
        return x * 2685821657736338717LL;
    }
};

#endif
