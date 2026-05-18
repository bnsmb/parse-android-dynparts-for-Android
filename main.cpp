/*
 * parse-android-dynparts - Android dynamic partition parser
 * Generates dmctl command files, optionally executes dmctl, and mounts devices.
 *
 * Compile with: clang++ -std=c++17 -o parse-android-dynparts main.cpp -llp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <fstream>
#include <sstream>
#include <set>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <liblp/liblp.h>

using namespace android::fs_mgr;

static void usage(const char* progname) {
    fprintf(stderr,
            "Usage: %s [OPTIONS] [<super_device>]\n"
            "Options:\n"
            "  -s, --slot N          Slot number (0 or 1) [default: current slot]\n"
            "  -o, --outdir DIR      Output directory for dmctl config files [default: .]\n"
            "  -p, --prefix PREFIX   Prefix to add to logical device names\n"
            "  -r, --rw              Create devices read-write (omit -ro flag)\n"
            "      --skip-cow        Ignore -cow partitions (cannot be mounted)\n"
            "      --partitions LIST Comma-separated list of partition names to process\n"
            "  -x, --execute         Execute dmctl for each generated config file\n"
            "      --delete          Delete config file after successful dmctl execution\n"
            "      --keep            Keep config file (default)\n"
            "      --mountdir DIR    Mount the logical device under DIR/<partname>\n"
            "                         (implies --execute; uses appropriate ro/rw flags)\n"
            "      --list            List partitions in the selected slot and exit\n"
            "      --list-all        List partitions in all slots and exit\n"
            "  -h, --help            Show this help\n"
            "\n"
            "If <super_device> is omitted, /dev/block/by-name/super is used.\n"
            "Environment variable DMCTL overrides dmctl binary path.\n"
            "Creates one file per partition: <outdir>/dmctl_<name>.txt\n",
            progname);
}

static int get_current_slot() {
    FILE* fp = popen("getprop ro.boot.slot_suffix 2>/dev/null", "r");
    if (!fp) return 0;
    char buf[16];
    if (fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        if (buf[0] == '_') {
            if (buf[1] == 'a') return 0;
            if (buf[1] == 'b') return 1;
        }
    } else {
        pclose(fp);
    }
    fp = fopen("/proc/cmdline", "r");
    if (fp) {
        char cmdline[512];
        if (fgets(cmdline, sizeof(cmdline), fp)) {
            char* token = strstr(cmdline, "androidboot.slot_suffix=");
            if (token) {
                token += strlen("androidboot.slot_suffix=");
                if (token[0] == '_') {
                    if (token[1] == 'a') return 0;
                    if (token[1] == 'b') return 1;
                }
            }
        }
        fclose(fp);
    }
    return 0;
}

static bool is_cow_partition(const std::string& name) {
    return name.size() >= 4 && name.substr(name.size() - 4) == "-cow";
}

static std::set<std::string> parse_partition_list(const std::string& list) {
    std::set<std::string> result;
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t start = item.find_first_not_of(" \t\r\n");
        size_t end = item.find_last_not_of(" \t\r\n");
        if (start != std::string::npos && end != std::string::npos)
            result.insert(item.substr(start, end - start + 1));
        else if (start != std::string::npos)
            result.insert(item.substr(start));
    }
    return result;
}

static bool device_exists(const std::string& dm_name) {
    std::string cmd = "dmctl list devices 2>/dev/null | grep -q '^" + dm_name + " '";
    return (system(cmd.c_str()) == 0);
}

static bool is_mounted(const std::string& mount_point) {
    std::string cmd = "grep -q ' ' " + mount_point + " ' /proc/mounts 2>/dev/null";
    return (system(cmd.c_str()) == 0);
}

static bool run_dmctl(const std::string& dmctl_path, const std::string& config_file) {
    pid_t pid = fork();
    if (pid == -1) { perror("fork"); return false; }
    if (pid == 0) {
        const char* args[] = { dmctl_path.c_str(), "-f", config_file.c_str(), nullptr };
        execvp(args[0], const_cast<char**>(args));
        perror("execvp");
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static bool mount_device(const std::string& device_name, const std::string& mount_point, bool read_only) {
    if (is_mounted(mount_point)) {
        std::cerr << "Mount point " << mount_point << " is already mounted. Skipping mount." << std::endl;
        return false;
    }
    std::string mount_options = read_only ? "ro" : "rw";
    std::string device_path = "/dev/block/mapper/" + device_name;

    if (mkdir(mount_point.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "Failed to create mount point " << mount_point << ": " << strerror(errno) << std::endl;
        return false;
    }

    pid_t pid = fork();
    if (pid == -1) { perror("fork"); return false; }
    if (pid == 0) {
        execlp("mount", "mount", "-t", "auto", "-o", mount_options.c_str(),
               device_path.c_str(), mount_point.c_str(), nullptr);
        perror("execlp mount");
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        std::cerr << "Mounted " << device_path << " on " << mount_point << " (" << mount_options << ")" << std::endl;
        return true;
    } else {
        std::cerr << "Mount failed with exit code " << WEXITSTATUS(status) << std::endl;
        return false;
    }
}

static void list_partitions(const std::unique_ptr<LpMetadata>& metadata, uint32_t slot_num) {
    std::cout << "Partitions in slot " << slot_num << ":" << std::endl;
    for (const auto& part : metadata->partitions) {
        std::string name = part.name;
        bool read_only = (part.attributes & LP_PARTITION_ATTR_READONLY) != 0;
        std::cout << "  " << name << (read_only ? " (ro)" : " (rw)");
        uint64_t total_sectors = 0;
        for (size_t i = 0; i < part.num_extents; ++i) {
            total_sectors += metadata->extents[part.first_extent_index + i].num_sectors;
        }
        std::cout << " - " << total_sectors << " sectors" << std::endl;
    }
}

int main(int argc, char** argv) {
    int slot = -1;
    std::string outdir = ".";
    std::string prefix = "";
    bool read_write = false;
    bool skip_cow = false;
    bool execute = false;
    bool delete_after = false;
    std::string mountdir = "";
    bool list_mode = false;
    bool list_all_mode = false;
    std::set<std::string> selected_partitions;

    static struct option long_options[] = {
        {"slot",        required_argument, 0, 's'},
        {"outdir",      required_argument, 0, 'o'},
        {"prefix",      required_argument, 0, 'p'},
        {"rw",          no_argument,       0, 'r'},
        {"skip-cow",    no_argument,       0, 1000},
        {"partitions",  required_argument, 0, 1001},
        {"execute",     no_argument,       0, 'x'},
        {"delete",      no_argument,       0, 1002},
        {"keep",        no_argument,       0, 1003},
        {"mountdir",    required_argument, 0, 1004},
        {"list",        no_argument,       0, 1005},
        {"list-all",    no_argument,       0, 1006},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c, option_index = 0;
    while ((c = getopt_long(argc, argv, "s:o:p:rxh", long_options, &option_index)) != -1) {
        switch (c) {
        case 's': slot = atoi(optarg); break;
        case 'o': outdir = optarg; break;
        case 'p': prefix = optarg; break;
        case 'r': read_write = true; break;
        case 1000: skip_cow = true; break;
        case 1001: selected_partitions = parse_partition_list(optarg); break;
        case 'x': execute = true; break;
        case 1002: delete_after = true; break;
        case 1003: break;
        case 1004: mountdir = optarg; break;
        case 1005: list_mode = true; break;
        case 1006: list_all_mode = true; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    if (list_mode && list_all_mode) {
        std::cerr << "Error: --list and --list-all are mutually exclusive." << std::endl;
        return 1;
    }

    // Determine super device path (default if not provided)
    std::string device_path_str = "/dev/block/by-name/super";
    if (optind < argc) {
        device_path_str = argv[optind];
    }
    const char* device_path = device_path_str.c_str();

    if (list_mode || list_all_mode) {
        if (list_all_mode) {
            for (uint32_t s = 0; s <= 1; ++s) {
                auto metadata = ReadMetadata(device_path, s);
                if (metadata) {
                    list_partitions(metadata, s);
                } else {
                    std::cerr << "Failed to read metadata for slot " << s << std::endl;
                }
            }
        } else {
            uint32_t slot_num;
            if (slot >= 0) slot_num = static_cast<uint32_t>(slot);
            else slot_num = static_cast<uint32_t>(get_current_slot());
            auto metadata = ReadMetadata(device_path, slot_num);
            if (!metadata) {
                std::cerr << "Failed to read metadata from " << device_path << " for slot " << slot_num << std::endl;
                return 1;
            }
            list_partitions(metadata, slot_num);
        }
        return 0;
    }

    if (!mountdir.empty()) execute = true;

    if (mkdir(outdir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "Warning: Could not create directory " << outdir << std::endl;
    }

    uint32_t slot_num;
    if (slot >= 0) slot_num = static_cast<uint32_t>(slot);
    else {
        slot_num = static_cast<uint32_t>(get_current_slot());
        std::cerr << "Auto-detected slot " << slot_num << std::endl;
    }

    std::unique_ptr<LpMetadata> metadata = ReadMetadata(device_path, slot_num);
    if (!metadata) {
        std::cerr << "Failed to read metadata from " << device_path << " for slot " << slot_num << std::endl;
        return 1;
    }

    std::set<std::string> available_names;
    for (const auto& part : metadata->partitions) {
        std::string name = part.name;
        if (skip_cow && is_cow_partition(name)) continue;
        available_names.insert(name);
    }

    if (!selected_partitions.empty()) {
        std::set<std::string> missing;
        for (const auto& req : selected_partitions) {
            if (available_names.find(req) == available_names.end())
                missing.insert(req);
        }
        if (!missing.empty()) {
            std::cerr << "Error: The following requested partitions do not exist in slot " << slot_num << ":";
            for (const auto& m : missing) std::cerr << " " << m;
            std::cerr << std::endl;
            return 1;
        }
    }

    std::string dmctl_path;
    const char* dmctl_env = getenv("DMCTL");
    if (dmctl_env && dmctl_env[0] != '\0') dmctl_path = dmctl_env;
    else dmctl_path = "dmctl";

    if (execute) {
        std::string test_cmd = dmctl_path + " help > /dev/null 2>&1";
        if (system(test_cmd.c_str()) != 0) {
            std::cerr << "dmctl not found or not executable: " << dmctl_path << std::endl;
            return 1;
        }
    }

    int file_count = 0, success_count = 0, fail_count = 0;

    for (const auto& part : metadata->partitions) {
        std::string orig_name = part.name;
        if (skip_cow && is_cow_partition(orig_name)) {
            std::cerr << "Skipping cow partition: " << orig_name << std::endl;
            continue;
        }
        if (!selected_partitions.empty() && selected_partitions.find(orig_name) == selected_partitions.end())
            continue;

        std::string dm_name = prefix + orig_name;
        bool read_only = !read_write && ((part.attributes & LP_PARTITION_ATTR_READONLY) != 0);

        std::vector<const LpMetadataExtent*> extents;
        for (size_t i = 0; i < part.num_extents; ++i)
            extents.push_back(&metadata->extents[part.first_extent_index + i]);
        if (extents.empty()) continue;

        std::string content;
        std::string cmd = "create " + dm_name;
        if (read_only) cmd += " -ro";

        const auto* first = extents[0];
        uint64_t logical_start = 0;
        char buf[512];
        snprintf(buf, sizeof(buf), " linear %llu %llu %s %llu",
                 (unsigned long long)logical_start,
                 (unsigned long long)first->num_sectors,
                 device_path,
                 (unsigned long long)first->target_data);
        cmd += buf;
        content += cmd + "\n";

        logical_start += first->num_sectors;
        for (size_t i = 1; i < extents.size(); ++i) {
            const auto* ext = extents[i];
            snprintf(buf, sizeof(buf), "linear %llu %llu %s %llu",
                     (unsigned long long)logical_start,
                     (unsigned long long)ext->num_sectors,
                     device_path,
                     (unsigned long long)ext->target_data);
            content += std::string(buf) + "\n";
            logical_start += ext->num_sectors;
        }

        std::string filename = outdir + "/dmctl_" + orig_name + ".txt";
        std::ofstream ofs(filename);
        if (!ofs) {
            std::cerr << "Error: Cannot write to " << filename << std::endl;
            fail_count++;
            continue;
        }
        ofs << content;
        ofs.close();
        std::cerr << "Created: " << filename << std::endl;
        file_count++;

        bool device_created = false;
        bool device_existed = false;

        if (execute) {
            if (device_exists(dm_name)) {
                std::cerr << "Device " << dm_name << " already exists. Skipping creation." << std::endl;
                device_existed = true;
            } else {
                std::cerr << "Executing: " << dmctl_path << " -f " << filename << std::endl;
                if (run_dmctl(dmctl_path, filename)) {
                    device_created = true;
                } else {
                    std::cerr << "Failed to create device for " << orig_name << std::endl;
                    fail_count++;
                    continue;
                }
            }
            success_count++;

            if (!mountdir.empty()) {
                std::string mount_point = mountdir + "/" + orig_name;
                if (!mount_device(dm_name, mount_point, read_only)) {
                    std::cerr << "Mount failed for " << dm_name << std::endl;
                    fail_count++;
                    success_count--;
                    continue;
                }
            }

            if (delete_after) {
                if (device_created || device_existed) {
                    if (unlink(filename.c_str()) == 0) {
                        std::cerr << "Deleted: " << filename << std::endl;
                    } else {
                        std::cerr << "Warning: Could not delete " << filename << std::endl;
                    }
                }
            }
        }
    }

    if (execute) {
        std::cerr << "Execution results: " << success_count << " succeeded, " << fail_count << " failed." << std::endl;
        return fail_count > 0 ? 1 : 0;
    } else {
        if (file_count == 0 && !selected_partitions.empty()) {
            std::cerr << "No matching partitions found (check slot and --skip-cow)." << std::endl;
            return 1;
        }
        std::cerr << "Done. " << file_count << " config file(s) written to " << outdir << std::endl;
        return 0;
    }
}

