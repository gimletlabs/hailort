/**
 * Copyright (c) 2020-2022 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
/**
 * @file filesystem.cpp
 * @brief Filesystem wrapper for Linux
 **/

#include "common/filesystem.hpp"
#include "common/logger_macros.hpp"
#include "common/utils.hpp"

#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>

namespace hailort
{

const char *Filesystem::SEPARATOR = "/";
const std::string UNIQUE_TMP_FILE_SUFFIX = "XXXXXX\0";

Expected<Filesystem::DirWalker> Filesystem::DirWalker::create(const std::string &dir_path)
{
    DIR *dir = opendir(dir_path.c_str());
    CHECK(nullptr != dir, make_unexpected(HAILO_FILE_OPERATION_FAILURE),
        "Could not open directory \"{}\" with errno {}", dir_path, errno);
    return DirWalker(dir, dir_path);
}

Filesystem::DirWalker::DirWalker(DIR *dir, const std::string &dir_path) :
    m_dir(dir),
    m_path_string(dir_path)
{}

Filesystem::DirWalker::~DirWalker()
{
    if (nullptr != m_dir) {
        const auto result = closedir(m_dir);
        if (-1 == result) {
            LOGGER__ERROR("closedir on directory \"{}\" failed with errno {}", m_path_string.c_str(), errno);
        }
    }
}

Filesystem::DirWalker::DirWalker(DirWalker &&other) :
    m_dir(std::exchange(other.m_dir, nullptr)),
    m_path_string(other.m_path_string)
{}
        
dirent* Filesystem::DirWalker::next_file()
{
    return readdir(m_dir);
}

#if defined(__linux__)

Expected<std::vector<std::string>> Filesystem::get_files_in_dir_flat(const std::string &dir_path)
{
    const std::string dir_path_with_sep = has_suffix(dir_path, SEPARATOR) ? dir_path : dir_path + SEPARATOR;
    
    auto dir = DirWalker::create(dir_path_with_sep);
    CHECK_EXPECTED(dir);
    
    std::vector<std::string> files;
    struct dirent *entry = nullptr;
    while ((entry = dir->next_file()) != nullptr) {
        if (entry->d_type != DT_REG) {
            continue;
        }
        const std::string file_name = entry->d_name;
        files.emplace_back(dir_path_with_sep + file_name);
    }

    return files;
}
// QNX
#elif defined(__QNX__)
Expected<std::vector<std::string>> Filesystem::get_files_in_dir_flat(const std::string &dir_path)
{
    (void) dir_path;
    return make_unexpected(HAILO_NOT_IMPLEMENTED);
}
// Unsupported Platform
#else
static_assert(false, "Unsupported Platform!");
#endif

Expected<time_t> Filesystem::get_file_modified_time(const std::string &file_path)
{
    struct stat attr;
    auto res = stat(file_path.c_str(), &attr);
    CHECK_AS_EXPECTED((0 == res), HAILO_INTERNAL_FAILURE, "stat() failed on file {}, with errno {}", file_path, errno);
    auto last_modification_time = attr.st_mtime;
    return last_modification_time;
}

#if defined(__linux__)

Expected<std::vector<std::string>> Filesystem::get_latest_files_in_dir_flat(const std::string &dir_path,
    std::chrono::milliseconds time_interval)
{
    std::time_t curr_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::string dir_path_with_sep = has_suffix(dir_path, SEPARATOR) ? dir_path : dir_path + SEPARATOR;
    
    auto dir = DirWalker::create(dir_path_with_sep);
    CHECK_EXPECTED(dir);
    
    std::vector<std::string> files;
    struct dirent *entry = nullptr;
    while ((entry = dir->next_file()) != nullptr) {
        if (entry->d_type != DT_REG) {
            continue;
        }

        const std::string file_path = dir_path_with_sep + std::string(entry->d_name);
        auto file_modified_time = get_file_modified_time(file_path);
        CHECK_EXPECTED(file_modified_time);

        auto time_diff_sec = std::difftime(curr_time, file_modified_time.value());
        auto time_diff_millisec = time_diff_sec * 1000;
        if (time_diff_millisec <= static_cast<double>(time_interval.count())) {
            files.emplace_back(file_path);
        }
    }

    return files;
}

#elif defined(__QNX__)
Expected<std::vector<std::string>> Filesystem::get_latest_files_in_dir_flat(const std::string &dir_path,
    std::chrono::milliseconds time_interval)
{
    // TODO: HRT-7643
    (void)dir_path;
    (void)time_interval;
    return make_unexpected(HAILO_NOT_IMPLEMENTED);
}
// Unsupported Platform
#else
static_assert(false, "Unsupported Platform!");
#endif // __linux__

Expected<bool> Filesystem::is_directory(const std::string &path)
{
    struct stat path_stat{};
    CHECK(0 == stat(path.c_str(), &path_stat), make_unexpected(HAILO_FILE_OPERATION_FAILURE),
        "stat() on path \"{}\" failed. errno {}", path.c_str(), errno);

   return S_ISDIR(path_stat.st_mode);
}

hailo_status Filesystem::create_directory(const std::string &dir_path)
{
    auto ret_val = mkdir(dir_path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO );
    CHECK((ret_val == 0) || (errno == EEXIST), HAILO_FILE_OPERATION_FAILURE, "Failed to create directory {}", dir_path);
    return HAILO_SUCCESS;
}

Expected<TempFile> TempFile::create(const std::string &file_name, const std::string &file_directory)
{
    if (!file_directory.empty()) {
        auto status = Filesystem::create_directory(file_directory);
        CHECK_SUCCESS_AS_EXPECTED(status);
    }

    std::string file_path = file_directory + file_name + UNIQUE_TMP_FILE_SUFFIX;
    char *fname = static_cast<char*>(std::malloc(sizeof(char) * (file_path.length() + 1)));
    std::strncpy(fname, file_path.c_str(), file_path.length() + 1);

    int fd = mkstemp(fname);
    CHECK_AS_EXPECTED((-1 != fd), HAILO_FILE_OPERATION_FAILURE, "Failed to create tmp file {}, with errno {}", file_path, errno);
    close(fd);

    return TempFile(fname);
}

TempFile::TempFile(const char *path) : m_path(path)
{}

TempFile::~TempFile()
{
    // TODO: Guarantee file deletion upon unexpected program termination. 
    std::remove(m_path.c_str());
}

std::string TempFile::name() const
{
    return m_path;
}

Expected<LockedFile> LockedFile::create(const std::string &file_path, const std::string &mode)
{
    auto fp = fopen(file_path.c_str(), mode.c_str());
    CHECK_AS_EXPECTED((nullptr != fp), HAILO_OPEN_FILE_FAILURE, "Failed opening file: {}, with errno: {}", file_path, errno);

    int fd = fileno(fp);
    int done = flock(fd, LOCK_EX | LOCK_NB);
    if (-1 == done) {
        LOGGER__ERROR("Failed to flock file: {}, with errno: {}", file_path, errno);
        fclose(fp);
        return make_unexpected(HAILO_FILE_OPERATION_FAILURE);
    }

    return LockedFile(fp, fd);
}

LockedFile::LockedFile(FILE *fp, int fd) : m_fp(fp), m_fd(fd)
{}

LockedFile::~LockedFile()
{
    if (-1 == flock(m_fd, LOCK_UN)) {
        LOGGER__ERROR("Failed to unlock file with errno {}", errno);
        fclose(m_fp);
    }
}

int LockedFile::get_fd() const
{
    return m_fd;
}

} /* namespace hailort */
