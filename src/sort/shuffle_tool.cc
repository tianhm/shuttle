#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <unistd.h>
#include <algorithm>
#include <set>
#include <sstream>
#include <iostream>
#include <gflags/gflags.h>
#include <map>
#include <boost/algorithm/string.hpp>
#include <boost/bind.hpp>
#include <boost/scoped_ptr.hpp>
#include "sort_file.h"
#include "logging.h"
#include "common/filesystem.h"
#include "common/tools_util.h"
#include "thread_pool.h"
#include "mutex.h"

DEFINE_int32(total, 0, "total numbers of map tasks");
DEFINE_int32(reduce_no, 0, "the reduce number of this reduce task");
DEFINE_string(work_dir, "/tmp", "the shuffle work dir");
DEFINE_int32(attempt_id, 0, "the attempt_id of this reduce task");
DEFINE_string(dfs_host, "", "host name of dfs master");
DEFINE_string(dfs_port, "", "port of dfs master");
DEFINE_string(dfs_user, "", "user name of dfs master");
DEFINE_string(dfs_password, "", "password of dfs master");
DEFINE_string(pipe, "streaming", "pipe style: streaming/bistreaming");
DEFINE_int32(tuo_size, 0, "one tuo contains how many maps'output");
DEFINE_int32(slow_start_no, 200, "if redcue_no greater than this, sleep a random time");

using baidu::common::Log;
using baidu::common::FATAL;
using baidu::common::INFO;
using baidu::common::WARNING;
using namespace baidu;
using namespace baidu::shuttle;

int32_t g_file_no(0);
FileSystem* g_fs(NULL);

void FillParam(FileSystem::Param& param) {
    if (!FLAGS_dfs_user.empty()) {
        param["user"] = FLAGS_dfs_user;
    }
    if (!FLAGS_dfs_password.empty()) {
        param["password"] = FLAGS_dfs_password;
    }
    if (!FLAGS_dfs_host.empty()) {
        param["host"] = FLAGS_dfs_host;
    }
    if (!FLAGS_dfs_port.empty()) {
        param["port"] = FLAGS_dfs_port;
    }
}

bool AddSortFiles(const std::string map_dir, std::vector<std::string>* file_names) {
    assert(file_names);
    std::vector<FileInfo> sort_files;
    if (g_fs->List(map_dir, &sort_files)) {
        std::vector<FileInfo>::iterator jt;
        for (jt = sort_files.begin(); jt != sort_files.end(); jt++) {
            const std::string& file_name = jt->name;
            if (boost::ends_with(file_name, ".sort")) {
                file_names->push_back(file_name);
            }
        }
        return true;
    } else {
        LOG(WARNING, "fail to list %s", map_dir.c_str());
        return false;
    }
}

bool MergeManyFilesToOne(const std::vector<std::string>& file_names,
                         const std::string& output_file) {
    MergeFileReader reader;
    FileSystem::Param param;
    FillParam(param);
    Status status = reader.Open(file_names, param, kHdfsFile);
    if (status != kOk) {
        LOG(WARNING, "fail to open: %s", reader.GetErrorFile().c_str());
        return false;
    }

    SortFileReader::Iterator* scan_it = reader.Scan("", "");
    boost::scoped_ptr<SortFileReader::Iterator> scan_it_guard(scan_it);

    if (scan_it->Error() != kOk && scan_it->Error() != kNoMore) {
        LOG(WARNING, "fail to scan: %s", reader.GetErrorFile().c_str());
        return false;
    }
    SortFileWriter* writer = SortFileWriter::Create(kHdfsFile, &status);
    boost::scoped_ptr<SortFileWriter> writer_guard(writer);

    if (status != kOk) {
        LOG(WARNING, "fail to create writer");
        return false;
    }
    FileSystem::Param param_write;
    FillParam(param_write);
    status = writer->Open(output_file, param_write);
    if (status != kOk) {
        LOG(WARNING, "fail to open %s for write", output_file.c_str());
        return false;
    }
    int64_t counter = 0;
    while (!scan_it->Done()) {
        status = writer->Put(scan_it->Key(), scan_it->Value());
        if (status != kOk) {
            LOG(WARNING, "fail to put: %s", output_file.c_str());
            return false;
        }
        counter++;
        if (counter % 5000 == 0) {
            LOG(INFO, "have written %lld records to %s",
                counter, output_file.c_str());
        }
        scan_it->Next();
        if (scan_it->Error() !=kOk && scan_it->Error() != kNoMore) {
            break;
        }
    }
    
    status = writer->Close();
    if (status != kOk) {
        LOG(WARNING, "fail to close writer: %s", output_file.c_str());
        reader.Close();
        return false;
    }
    status = reader.Close();
    if (status != kOk) {
        LOG(WARNING, "fail to close reader: %s", reader.GetErrorFile().c_str());
        return false;
    }
    if (scan_it->Error() != kOk && scan_it->Error() != kNoMore) {
        LOG(WARNING, "fail to scan: %s", reader.GetErrorFile().c_str());
        return false;
    }
    LOG(INFO, "totally written %lld records to %s",
        counter, output_file.c_str());
    return true;
}

void MergeAndPrint(const std::vector<std::string>& file_names) {
    MergeFileReader reader;
    FileSystem::Param param;
    FillParam(param);
    Status status = reader.Open(file_names, param, kHdfsFile);
    if (status != kOk) {
        LOG(WARNING, "fail to open: %s", reader.GetErrorFile().c_str());
        _exit(1);
    }
    char s_reduce_no[256];
    snprintf(s_reduce_no, sizeof(s_reduce_no), "%05d", FLAGS_reduce_no);
    std::string s_reduce_key(s_reduce_no);
    SortFileReader::Iterator* scan_it = reader.Scan(s_reduce_key,
                                                    s_reduce_key + "\xff");
    if (scan_it->Error() != kOk && scan_it->Error() != kNoMore) {
        LOG(WARNING, "fail to scan: %s", reader.GetErrorFile().c_str());
        _exit(2);
    }
    while (!scan_it->Done()) {
        if (FLAGS_pipe == "streaming") {
            const std::string& line = scan_it->Value();
            if (!line.empty()) {
                std::cout << line << std::endl;
            }
        } else {
            std::cout << scan_it->Value();
        }
        scan_it->Next();
    }
    if (scan_it->Error() != kOk && scan_it->Error() != kNoMore) {
        LOG(WARNING, "fail to scan: %s", reader.GetErrorFile().c_str());
        _exit(3);
    }
    reader.Close();
    delete scan_it;
}

bool MergeOneTuo(int map_from, int map_to, int tuo_now) {
    std::vector<std::string> file_names;
    for (int i = map_from; i <= map_to; i++) {
        std::stringstream ss;
        ss << FLAGS_work_dir << "/map_" << i;
        const std::string& map_dir = ss.str();
        if (!AddSortFiles(map_dir, &file_names) || file_names.empty()) {
            return false;
        }
    }
    char tuo_dir[4096];
    snprintf(tuo_dir, sizeof(tuo_dir), "%s/tuo_%d_%d",
             FLAGS_work_dir.c_str(), FLAGS_reduce_no, FLAGS_attempt_id);
    g_fs->Mkdirs(tuo_dir);
    char output_file[4096];
    snprintf(output_file, sizeof(output_file), "%s/tuo_%d_%d/%d.tuo",
            FLAGS_work_dir.c_str(), FLAGS_reduce_no, FLAGS_attempt_id, tuo_now);
    if (!MergeManyFilesToOne(file_names, output_file)) {
        return false;
    }
    std::stringstream ss;
    ss << FLAGS_work_dir << "/" << tuo_now << ".tuo";
    const std::string real_tuo_name = ss.str();
    if (!g_fs->Rename(output_file, real_tuo_name)) {
        g_fs->Remove(output_file);
        return false;
    }
    std::vector<std::string>::iterator it;
    for (it = file_names.begin(); it != file_names.end(); it++) {
        g_fs->Remove(*it);
    }
    return true;
}

int MergeTuo() {
    srand(time(0));
    int n_tuo = (int)ceil((float)FLAGS_total / FLAGS_tuo_size) ;
    LOG(INFO, "will merge %d tuo", n_tuo);
    std::vector<int> tuo_list;
    for (int i = 0; i < n_tuo; i++) {
        tuo_list.push_back(i);
    }
    std::random_shuffle(tuo_list.begin(), tuo_list.end());
    std::set<int> ready_tuo_set;
    if (FLAGS_reduce_no < n_tuo) {
        while (ready_tuo_set.empty()) { //at first, merge tuo belongs to me!
            int tuo_now = FLAGS_reduce_no;
            std::stringstream ss;
            ss << FLAGS_work_dir << "/" << tuo_now << ".tuo";
            const std::string& tuo_file_name = ss.str();
            if (g_fs->Exist(tuo_file_name)) {
                ready_tuo_set.insert(tuo_now);
                LOG(INFO, "lucky, my tuo ready, total #%d/%d tuo ready",
                    ready_tuo_set.size(), n_tuo);
                continue;
            }
            int map_from = tuo_now * FLAGS_tuo_size;
            int map_to = std::min( (tuo_now + 1) * FLAGS_tuo_size - 1, FLAGS_total - 1);
            LOG(INFO, "merge tuo from %d to %d", map_from, map_to);
            if (MergeOneTuo(map_from, map_to, tuo_now)) {
                ready_tuo_set.insert(tuo_now);
                LOG(INFO, "my tuo done. total #%d/%d tuo ready", ready_tuo_set.size(), n_tuo);
            }
            sleep(5);
        }
    }

    while (ready_tuo_set.size() < (size_t)n_tuo) {
        std::vector<int>::iterator it;
        for (it = tuo_list.begin(); it != tuo_list.end(); it++) {
            int tuo_now = *it;
            if (ready_tuo_set.find(tuo_now) != ready_tuo_set.end()) {
                continue;
            }
            std::stringstream ss;
            ss << FLAGS_work_dir << "/" << tuo_now << ".tuo";
            const std::string& tuo_file_name = ss.str();
            if (g_fs->Exist(tuo_file_name)) {
                ready_tuo_set.insert(tuo_now);
                LOG(INFO, "lucky, total #%d/%d tuo ready", ready_tuo_set.size(), n_tuo);
                continue;
            }
            if (FLAGS_reduce_no > n_tuo * 2) {
                continue;
            }
            std::stringstream ss_lock;
            std::stringstream my_lock_flag;
            ss_lock << FLAGS_work_dir << "/tuo_lock_" << tuo_now << "/";
            std::vector<baidu::shuttle::FileInfo> lockers;
            g_fs->List(ss_lock.str(), &lockers);
            my_lock_flag << ss_lock.str() << FLAGS_reduce_no;
            if (lockers.size() > 2 && !g_fs->Exist(my_lock_flag.str())) {
                LOG(WARNING, "two many workers on this tuo!: %d", tuo_now);
                double rn = rand() / (RAND_MAX+0.0);
                if (rn < 0.99) {
                    sleep(1);
                    continue;
                }
            }
            if (!g_fs->Exist(my_lock_flag.str()) &&
                g_fs->Exist(FLAGS_work_dir)) {
                g_fs->Open(my_lock_flag.str(), kWriteFile);
                g_fs->Close(); //create my lock
            }
            int map_from = tuo_now * FLAGS_tuo_size;
            int map_to = std::min( (tuo_now + 1) * FLAGS_tuo_size - 1, FLAGS_total - 1);
            LOG(INFO, "merge tuo from %d to %d", map_from, map_to);
            if (MergeOneTuo(map_from, map_to, tuo_now)) {
                ready_tuo_set.insert(tuo_now);
                LOG(INFO, "total #%d/%d tuo ready", ready_tuo_set.size(), n_tuo);
                g_fs->Remove(ss_lock.str());
            } else {
                g_fs->Remove(my_lock_flag.str());
            }
            sleep(3);
        } // end of for
        sleep(5);
    }// end of while
    return n_tuo;
}

int main(int argc, char* argv[]) {
    baidu::common::SetLogFile(GetLogName("./shuffle_tool.log").c_str());
    baidu::common::SetWarningFile(GetLogName("./shuffle_tool.log.wf").c_str());
    google::ParseCommandLineFlags(&argc, &argv, true);
    FileSystem::Param param;
    FillParam(param);
    g_fs = FileSystem::CreateInfHdfs(param);
    if (FLAGS_total == 0 ) {
        LOG(FATAL, "invalid map task total");
    }
    if (FLAGS_tuo_size == 0) {
        FLAGS_tuo_size = std::min((int32_t)ceil(sqrt(FLAGS_total)), 300);
        int n_tuo = (int)ceil((float)FLAGS_total / FLAGS_tuo_size);
        if (n_tuo < 100) {
            FLAGS_tuo_size = std::max((int32_t)ceil(sqrt(FLAGS_tuo_size)), 10);
        }
    }
    LOG(INFO, "tuo_size: %d", FLAGS_tuo_size);
    int n_tuo = MergeTuo();
    std::vector<std::string>  tuo_file_names;
    for (int i = 0;  i< n_tuo; i++) {
        std::stringstream ss;
        ss << FLAGS_work_dir + "/" << i << ".tuo";
        tuo_file_names.push_back(ss.str());
    }
    if (FLAGS_reduce_no > FLAGS_slow_start_no) {
        double rn = rand() / (RAND_MAX+0.0);
        int random_period = static_cast<int>(rn * 90);
        LOG(INFO, "sleep a random time: %d", random_period);
        sleep(random_period);
    }
    MergeAndPrint(tuo_file_names);
    return 0;
}
