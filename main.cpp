/*
 * mount_dynamic_partitions - Android dynamic partition manager
 * Version 1.0.0
 *
 * Creates dmctl devices and optionally mounts partitions.
 *
 * Compile with: clang++ -std=c++17 -o mount_dynamic_partitions main.cpp -llp
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
#include <fcntl.h>
#include <limits.h>
#include <liblp/liblp.h>

#define VERSION "1.0.0"

using namespace android::fs_mgr;

static void usage(const char* progname) {
    fprintf(stderr,
            "Usage: %s [OPTIONS] [<super_device>]\n"
            "Version: %s\n\n"
            "Options:\n"
            "  -s, --slot N          Slot number (0 or 1) [default: current slot]\n"
            "  -o, --outdir DIR      Output directory for dmctl config files [default: .]\n"
            "  -p, --prefix PREFIX   Prefix to add to logical device names\n"
            "  -r, --rw              Create devices read-write (omit -ro flag)\n"
            "      --skip-cow        Ignore -cow partitions\n"
            "      --partitions LIST Comma-separated list of partition names\n"
            "  -x, --execute         Execute dmctl for each config file\n"
            "      --dry-run         Generate script without executing (implies -x)\n"
            "      --delete          Delete config file after successful execution\n"
            "      --keep            Keep config file (default)\n"
            "      --mountdir DIR    Mount devices under DIR/<partname>\n"
            "      --gen-scripts PFX Generate unmount/remove scripts with prefix PFX\n"
            "      --force-multi-rw  Allow multiple read-write mounts of same device\n"
            "      --list            List partitions in selected slot\n"
            "      --list-all        List partitions in all slots\n"
            "  -V, --version         Print version and exit\n"
            "  -h, --help            Show this help\n"
            "\n"
            "Default super device: /dev/block/by-name/super\n"
            "Environment DMCTL overrides dmctl path.\n",
            progname, VERSION);
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
    std::string path = "/dev/block/mapper/" + dm_name;
    struct stat st;
    return (stat(path.c_str(), &st) == 0);
}

static bool is_mounted(const std::string& mount_point) {
    char abs_path[PATH_MAX];
    if (realpath(mount_point.c_str(), abs_path) == nullptr) {
        return false; // Path doesn't exist or can't be resolved -> not mounted
    }
    std::ifstream mounts("/proc/mounts");
    if (!mounts.is_open()) return false;
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string dev, mp, fstype;
        if (iss >> dev >> mp >> fstype) {
            if (mp == abs_path) return true;
        }
    }
    return false;
}

static bool wait_for_device(const std::string& device_name, int timeout_sec = 2) {
    std::string path = "/dev/block/mapper/" + device_name;
    for (int i = 0; i < timeout_sec * 10; ++i) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) return true;
        usleep(100000); // 100 ms
    }
    return false;
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
        std::cerr << "Mount point " << mount_point << " already mounted. Skipping." << std::endl;
        return true;
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
        // Suppress mount error output for "need -t"
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("mount", "mount", "-t", "auto", "-o", mount_options.c_str(),
               device_path.c_str(), mount_point.c_str(), nullptr);
        perror("execlp mount");
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    int exit_code = WEXITSTATUS(status);
    if (WIFEXITED(status) && exit_code == 0) {
        std::cerr << "Mounted " << device_path << " on " << mount_point << " (" << mount_options << ")" << std::endl;
        return true;
    } else if (exit_code == 1) {
        // No filesystem, ignore (error message suppressed)
        return true;
    } else {
        std::cerr << "Mount failed with exit code " << exit_code << std::endl;
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
        for (size_t i = 0; i < part.num_extents; ++i)
            total_sectors += metadata->extents[part.first_extent_index + i].num_sectors;
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
    bool dry_run = false;
    bool delete_after = false;
    std::string mountdir = "";
    std::string gen_scripts_prefix = "";
    bool force_multi_rw = false;
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
        {"dry-run",     no_argument,       0, 1008},
        {"delete",      no_argument,       0, 1002},
        {"keep",        no_argument,       0, 1003},
        {"mountdir",    required_argument, 0, 1004},
        {"gen-scripts", required_argument, 0, 1007},
        {"force-multi-rw", no_argument,   0, 1009},
        {"list",        no_argument,       0, 1005},
        {"list-all",    no_argument,       0, 1006},
        {"version",     no_argument,       0, 'V'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c, option_index = 0;
    while ((c = getopt_long(argc, argv, "s:o:p:rxVh", long_options, &option_index)) != -1) {
        switch (c) {
        case 's': slot = atoi(optarg); break;
        case 'o': outdir = optarg; break;
        case 'p': prefix = optarg; break;
        case 'r': read_write = true; break;
        case 1000: skip_cow = true; break;
        case 1001: selected_partitions = parse_partition_list(optarg); break;
        case 'x': execute = true; break;
        case 1008: dry_run = true; break;
        case 1002: delete_after = true; break;
        case 1003: break;
        case 1004: mountdir = optarg; break;
        case 1007: gen_scripts_prefix = optarg; break;
        case 1009: force_multi_rw = true; break;
        case 1005: list_mode = true; break;
        case 1006: list_all_mode = true; break;
        case 'V': std::cout << "mount_dynamic_partitions version " << VERSION << std::endl; return 0;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    if (dry_run) execute = true;

    if (list_mode && list_all_mode) {
        std::cerr << "Error: --list and --list-all are mutually exclusive." << std::endl;
        return 1;
    }

    std::string device_path_str = "/dev/block/by-name/super";
    if (optind < argc) device_path_str = argv[optind];
    const char* device_path = device_path_str.c_str();

    if (list_mode || list_all_mode) {
        if (list_all_mode) {
            for (uint32_t s = 0; s <= 1; ++s) {
                auto metadata = ReadMetadata(device_path, s);
                if (metadata) list_partitions(metadata, s);
                else std::cerr << "Failed to read metadata for slot " << s << std::endl;
            }
        } else {
            uint32_t slot_num = (slot >= 0) ? static_cast<uint32_t>(slot) : static_cast<uint32_t>(get_current_slot());
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

    uint32_t slot_num = (slot >= 0) ? static_cast<uint32_t>(slot) : static_cast<uint32_t>(get_current_slot());
    if (slot < 0) std::cerr << "Auto-detected slot " << slot_num << std::endl;

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
        for (const auto& req : selected_partitions)
            if (available_names.find(req) == available_names.end()) missing.insert(req);
        if (!missing.empty()) {
            std::cerr << "Error: Requested partitions not in slot " << slot_num << ":";
            for (const auto& m : missing) std::cerr << " " << m;
            std::cerr << std::endl;
            return 1;
        }
    }

    std::string dmctl_path;
    const char* dmctl_env = getenv("DMCTL");
    if (dmctl_env && dmctl_env[0] != '\0') dmctl_path = dmctl_env;
    else dmctl_path = "dmctl";

    if (execute && !dry_run) {
        std::string test_cmd = dmctl_path + " help > /dev/null 2>&1";
        if (system(test_cmd.c_str()) != 0) {
            std::cerr << "dmctl not found: " << dmctl_path << std::endl;
            return 1;
        }
    }

    // For script generation, collect succeeded mounts and devices
    std::vector<std::string> mounted_points;
    std::vector<std::string> created_devices;
    std::set<std::string> rw_mounted_devices; // track devices mounted RW

    int success_count = 0, fail_count = 0;

    // If dry-run, we'll generate a shell script with all commands
    std::ofstream dryrun_script;
    if (dry_run) {
        std::string script_path = outdir + "/mount_all.sh";
        dryrun_script.open(script_path);
        if (dryrun_script.is_open()) {
            dryrun_script << "#!/system/bin/sh\n";
            dryrun_script << "# Auto-generated script to create and mount dynamic partitions\n";
            dryrun_script << "# Generated by mount_dynamic_partitions\n\n";
            dryrun_script << "set -e\n\n";
            dryrun_script << "DMCTL=" << dmctl_path << "\n\n";
        } else {
            std::cerr << "Warning: Could not create dry-run script " << script_path << std::endl;
        }
    }

    for (const auto& part : metadata->partitions) {
        std::string orig_name = part.name;
        if (skip_cow && is_cow_partition(orig_name)) continue;
        if (!selected_partitions.empty() && selected_partitions.find(orig_name) == selected_partitions.end()) continue;

        std::string dm_name = prefix + orig_name;
        bool read_only = !read_write && ((part.attributes & LP_PARTITION_ATTR_READONLY) != 0);

        // Check for multiple RW mounts
        if (!read_only && !force_multi_rw && rw_mounted_devices.find(dm_name) != rw_mounted_devices.end()) {
            std::cerr << "Warning: Device " << dm_name << " already mounted read-write. Skipping additional RW mount." << std::endl;
            continue;
        }

        std::vector<const LpMetadataExtent*> extents;
        for (size_t i = 0; i < part.num_extents; ++i)
            extents.push_back(&metadata->extents[part.first_extent_index + i]);
        if (extents.empty()) continue;

        // Build dmctl config file content
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

        // Write to dry-run script if active
        if (dryrun_script.is_open()) {
            dryrun_script << "\n# Logical device: " << orig_name << "\n";
            dryrun_script << "cat > " << filename << " << 'EOF'\n";
            dryrun_script << content;
            dryrun_script << "EOF\n";
        }

        if (execute) {
            bool device_existed = false;
            if (!dry_run) {
                device_existed = device_exists(dm_name);
            } else {
                device_existed = false;
            }

            if (device_existed) {
                std::cerr << "Device " << dm_name << " already exists." << std::endl;
            } else {
                if (dry_run) {
                    std::cerr << "DRY RUN: Would execute: " << dmctl_path << " -f " << filename << std::endl;
                    if (dryrun_script.is_open()) {
                        dryrun_script << "$DMCTL -f " << filename << "\n";
                    }
                } else {
                    std::cerr << "Executing: " << dmctl_path << " -f " << filename << std::endl;
                    if (run_dmctl(dmctl_path, filename)) {
                        if (!wait_for_device(dm_name)) {
                            std::cerr << "Device " << dm_name << " did not appear after creation." << std::endl;
                            fail_count++;
                            continue;
                        }
                    } else {
                        std::cerr << "Failed to create device for " << orig_name << std::endl;
                        fail_count++;
                        continue;
                    }
                }
            }

            if (!device_existed) created_devices.push_back(dm_name);

            success_count++;

            if (!mountdir.empty()) {
                std::string mount_point = mountdir + "/" + orig_name;
                bool mount_success = false;
                if (dry_run) {
                    std::cerr << "DRY RUN: Would mount " << dm_name << " on " << mount_point << " (" << (read_only ? "ro" : "rw") << ")" << std::endl;
                    if (dryrun_script.is_open()) {
                        dryrun_script << "mkdir -p " << mount_point << "\n";
                        dryrun_script << "mount -t auto -o " << (read_only ? "ro" : "rw") << " /dev/block/mapper/" << dm_name << " " << mount_point << "\n";
                    }
                    mount_success = true;
                } else {
                    mount_success = mount_device(dm_name, mount_point, read_only);
                }
                if (mount_success) {
                    mounted_points.push_back(mount_point);
                    if (!read_only) rw_mounted_devices.insert(dm_name);
                } else {
                    fail_count++;
                    success_count--;
                }
            }

            if (!dry_run && delete_after) {
                if (unlink(filename.c_str()) == 0)
                    std::cerr << "Deleted: " << filename << std::endl;
                else
                    std::cerr << "Warning: Could not delete " << filename << std::endl;
            } else if (dry_run && delete_after && dryrun_script.is_open()) {
                dryrun_script << "rm -f " << filename << "\n";
            }

            std::cout << std::endl;
        }
    }

    if (dryrun_script.is_open()) {
        dryrun_script << "\necho \"All operations completed.\"\n";
        dryrun_script.close();
        chmod((outdir + "/mount_all.sh").c_str(), 0755);
        std::cerr << "Generated executable script: " << outdir << "/mount_all.sh" << std::endl;
    }

    // Generate unmount and remove scripts if requested (even in dry-run)
    if (!gen_scripts_prefix.empty()) {
        std::string umount_script = gen_scripts_prefix + "_umount.sh";
        std::string remove_script = gen_scripts_prefix + "_remove.sh";

        std::ofstream umount_fs(umount_script);
        if (umount_fs.is_open()) {
            umount_fs << "#!/system/bin/sh\n";
            umount_fs << "# Unmount all partitions mounted by mount_dynamic_partitions\n\n";
            for (const auto& mp : mounted_points) {
                umount_fs << "umount " << mp << " 2>/dev/null\n";
                umount_fs << "rmdir " << mp << " 2>/dev/null\n";
            }
            umount_fs.close();
            if (!dry_run) chmod(umount_script.c_str(), 0755);
            std::cerr << "Generated unmount script: " << umount_script << std::endl;
        } else {
            std::cerr << "Error: Could not create " << umount_script << std::endl;
        }

        std::ofstream remove_fs(remove_script);
        if (remove_fs.is_open()) {
            remove_fs << "#!/system/bin/sh\n";
            remove_fs << "# Remove all logical devices created by mount_dynamic_partitions\n\n";
            for (const auto& dev : created_devices) {
                remove_fs << dmctl_path << " remove " << dev << " 2>/dev/null\n";
            }
            remove_fs.close();
            if (!dry_run) chmod(remove_script.c_str(), 0755);
            std::cerr << "Generated removal script: " << remove_script << std::endl;
        } else {
            std::cerr << "Error: Could not create " << remove_script << std::endl;
        }
    }

    if (dry_run) {
        std::cerr << "DRY RUN completed. No changes were made." << std::endl;
    }

    if (execute) {
        std::cerr << "Execution results: " << success_count << " succeeded, " << fail_count << " failed." << std::endl;
        return (fail_count > 0 && !dry_run) ? 1 : 0;
    } else {
        std::cerr << "Config files written to " << outdir << std::endl;
        return 0;
    }
}
