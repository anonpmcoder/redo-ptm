#include <algorithm>
#include <set>
#include <string>
#include <stdio.h>
#include <vector>
#include "../db/db_impl.h"
#include "../db/db.h"
#include "../db/options.h"


namespace ptmdb {

Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr) {
    *dbptr = new DBImpl(options, dbname);
    return Status::OK();
}

Status DestroyDB(const std::string& dbname, const Options& options) {
    //RomulusLog::init(); // TODO: fix this
    //RomulusLog::reset();
    return Status::OK();
}

}  // namespace ptmdb

