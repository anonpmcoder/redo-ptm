#include <iostream>
#include <fstream>
#include <cstring>

#include "pdatastructures/TMRedBlackTree.hpp"
#include "pdatastructures/TMRedBlackTreeByRef.hpp"
#ifdef USE_CXPTM
#include "ptms/cxptm/CXPTM.hpp"
#define DATA_FILE "data/pset-tree-1m-dedicated-cxptm.txt"
#elif defined USE_CXPUC
#include "ptms/cxpuc/CXPUC.hpp"
#include "ptms/cxpuc/PUCSet.hpp"
#include "pdatastructures/psequential/PRedBlackTree.hpp"
#define DATA_FILE "data/pset-tree-1m-dedicated-cxpuc.txt"
#elif defined USE_REDO
#include "ptms/redo/Redo.hpp"
#define DATA_FILE "data/pset-tree-1m-dedicated-redo.txt"
#elif defined USE_REDOTIMED
#include "ptms/redotimed/RedoTimed.hpp"
#define DATA_FILE "data/pset-tree-1m-dedicated-redotimed.txt"
#elif defined USE_REDOOPT
#include "ptms/redoopt/RedoOpt.hpp"
#define DATA_FILE "data/pset-tree-1m-dedicated-redoopt.txt"
#elif defined USE_OFWF
#include "ptms/ponefilewf/OneFilePTMWF.hpp"
#define DATA_FILE "data/pset-tree-1m-dedicated-ofwf.txt"
#elif defined USE_PMDK
#include "ptms/pmdk/PMDKTM.hpp"
#define DATA_FILE "data/pset-tree-1m-dedicated-pmdk.txt"
#endif
#include "PBenchmarkSets.hpp"


int main(void) {
    const std::string dataFilename { DATA_FILE };
    vector<int> threadList = { 1, 2, 4, 8, 16, 24, 32, 40}; // For castor-1
    vector<int> ratioList = { 0 };                                   // threadList indicates number of read-only threads, and there are two more write-only threads
    const int numElements = 1000*1000;                               // Number of keys in the set
    const int numRuns = 1;                                           // 5 runs for the paper
    const seconds testLength = 20s;                                  // 20s for the paper
    const int EMAX_CLASS = 10;
    uint64_t results[EMAX_CLASS][threadList.size()][ratioList.size()];
    std::string cNames[EMAX_CLASS];
    int maxClass = 0;
    // Reset results
    std::memset(results, 0, sizeof(uint64_t)*EMAX_CLASS*threadList.size()*ratioList.size());

    double totalHours = (double)ratioList.size()*threadList.size()*testLength.count()*numRuns/(60.*60.);
    std::cout << "This benchmark is going to take " << totalHours << " hours to complete\n";
#ifdef USE_PMDK
    std::cout << "If you use PMDK on /dev/shm/, don't forget to set 'export PMEM_IS_PMEM_FORCE=1'\n";
#endif

    PBenchmarkSets<uint64_t> bench;
    for (unsigned ir = 0; ir < ratioList.size(); ir++) {
        auto ratio = ratioList[ir];
        for (unsigned it = 0; it < threadList.size(); it++) {
            auto nThreads = threadList[it];
            int ic = 0;
            std::cout << "\n----- Persistent Sets (Red-Black Tree) DEDICATED   numElements=" << numElements << "   ratio=" << ratio/10. << "%   threads=" << nThreads << "   runs=" << numRuns << "   length=" << testLength.count() << "s -----\n";
#ifdef USE_CXPTM
            results[ic][it][ir] = bench.benchmark<TMRedBlackTree<uint64_t,uint64_t,cx::CX,cx::persist>,                              cx::CX>                  (cNames[ic], nThreads, ratio, testLength, numRuns, numElements, true);
            ic++;
#elif defined USE_CXPUC
            results[ic][it][ir] = bench.benchmarkPUC<PUCSet<PRedBlackTree<uint64_t,uint64_t,cxpuc::Allocator>,uint64_t>>                                      (cNames[ic], nThreads, ratio, testLength, numRuns, numElements, true);
            ic++;
#elif defined USE_REDO
            results[ic][it][ir] = bench.benchmark<TMRedBlackTree<uint64_t,uint64_t,redo::Redo,redo::persist>, redo::Redo>          (cNames[ic], nThreads, ratio, testLength, numRuns, numElements, true);
            ic++;
#elif defined USE_REDOTIMED
            results[ic][it][ir] = bench.benchmark<TMRedBlackTree<uint64_t,uint64_t,redotimed::RedoTimed,redotimed::persist>, redotimed::RedoTimed>(cNames[ic], nThreads, ratio, testLength, numRuns, numElements, true);
            ic++;
#elif defined USE_REDOOPT
            results[ic][it][ir] = bench.benchmark<TMRedBlackTree<uint64_t,uint64_t,redoopt::RedoOpt,redoopt::persist>,   redoopt::RedoOpt>(cNames[ic], nThreads, ratio, testLength, numRuns, numElements, true);
            ic++;
#elif defined USE_OFWF
            results[ic][it][ir] = bench.benchmark<TMRedBlackTree<uint64_t,uint64_t,onefileptmwf::OneFileWF,onefileptmwf::tmtype>,    onefileptmwf::OneFileWF> (cNames[ic], nThreads, ratio, testLength, numRuns, numElements, true);
            ic++;
#elif defined USE_PMDK
            results[ic][it][ir] = bench.benchmark<TMRedBlackTreeByRef<uint64_t,uint64_t,pmdk::PMDKTM,pmdk::persist>,                 pmdk::PMDKTM>            (cNames[ic], nThreads, ratio, testLength, numRuns, numElements, true);
            ic++;
#endif
            maxClass = ic;
        }
    }

    // Export tab-separated values to a file to be imported in gnuplot or excel
    ofstream dataFile;
    dataFile.open(dataFilename);
    dataFile << "Threads\t";
    // Printf class names and ratios for each column
    for (unsigned ir = 0; ir < ratioList.size(); ir++) {
        auto ratio = ratioList[ir];
        for (int ic = 0; ic < maxClass; ic++) dataFile << cNames[ic] << "-" << ratio/10. << "%"<< "\t";
    }
    dataFile << "\n";
    for (int it = 0; it < threadList.size(); it++) {
        dataFile << threadList[it] << "\t";
        for (unsigned ir = 0; ir < ratioList.size(); ir++) {
            for (int ic = 0; ic < maxClass; ic++) dataFile << results[ic][it][ir] << "\t";
        }
        dataFile << "\n";
    }
    dataFile.close();
    std::cout << "\nSuccessfuly saved results in " << dataFilename << "\n";

    return 0;
}
