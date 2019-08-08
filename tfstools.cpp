#include "tfstools.h"
#include "gptpart.h"
#include "fat32.h"

#include <stdio.h>
#include <errno.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <memory>
#include <cstring>
#include <map>
#include <cctype>
#include <functional>
#include <ctime>
#include <cstdlib>
#include <algorithm>

#define STR(x) #x

inline void GUID2Text(GPTGUID guid, char *buf) {
    sprintf(buf, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
    guid[3], guid[2], guid[1], guid[0],    //part1
    guid[5], guid[4],                      //part2
    guid[7], guid[6],                      //part3
    guid[8], guid[9],                      //part4
    guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);  //part5
}

inline void Text2GUID(const char *text, GPTGUID *guid) {
    auto f = [](const char *s) {
        unsigned char r = 0;    
        for(int i = 0; i < 2; i++) {
            if(s[i] >= '0' && s[i] <= '9')
                r += (s[i]-'0') * (i == 0 ? 16 : 1);
            else if(s[i] >= 'A' && s[i] <= 'F')
                r += (s[i]-'A'+10) * (i == 0 ? 16 : 1);
        }
        return r;
    };
    GPTGUID &g = *guid;
    g[0] = f(text+6);
    g[1] = f(text+4);
    g[2] = f(text+2);
    g[3] = f(text);
    g[4] = f(text+11);
    g[5] = f(text+9);
    g[6] = f(text+16);
    g[7] = f(text+14);
    g[8] = f(text+19);
    g[9] = f(text+21);
    g[10] = f(text+24);
    g[11] = f(text+26);
    g[12] = f(text+28);
    g[13] = f(text+30);
    g[14] = f(text+32);
    g[15] = f(text+34);
}

struct {
    FILE *fp;
    bool readonly;
    union {
        GPTPartTool gpt_part_tool;     
    };
    PartTool *part_tool;  //目前来说是钦点使用gpt_part_tool了
    FSHandle pt_handle;

    int phase; 
    PartInfo info;
#define PHASE_FREE                  0
#define PHASE_CREATE_PARTITION      1
    
    //文件系统工具
    union {
        FAT32Tool fat32_tool;
        //TODO: 添加对其它的文件系统的支持
    };
    FSTool *fs_tool;      //切换fs_tool指针指向上面联合体内的某个成员，调用统一的接口，实现类似C++中多态的效果。
    FSHandle fs_handle;   //指向当前目录的文件系统句柄
    FSHandle pdir_handle; //上级目录的句柄
    std::map<std::string, std::pair<FileHandle, FileInfo>> lookup;   //用于记录当前目录下的文件/子目录 名字对应的句柄和信息 
}g_state;
FILE *&g_fp = g_state.fp;

extern "C" {

uint64 read_lba_stdio(uint64 start, uint64 end, void *buf) {
    fseek(g_fp, start * 512, SEEK_SET);
    return fread(buf, 1, (end-start+1) * 512, g_fp) / 512; 
}

uint64 write_lba_stdio(uint64 start, uint64 end, void *buf) {
    fseek(g_fp, start * 512, SEEK_SET);
    return fwrite(buf, 1, (end-start+1) * 512, g_fp);
}

}

FSIO g_io = { read_lba_stdio, write_lba_stdio };

static const char *help_msg = STR(
Taurix Filesystem Tools\n
\tUsage: tfstools script_file
);

#define TYPE_GUID_REFERENCE STR(\n\tC12A7328-F81F-11D2-BA4B-00A0C93EC93B : EFI System \
\n\tEBD0A0A2-B9E5-4433-87C0-68B6B72699C7 : Data \
\n\t0657FD6D-A4AB-43C4-84E5-0933C84B4F4F : Linux Swapfile \
\n\tDE94BBA4-06D1-4D40-A16A-BFD50179D6AC : Windows Recovery Environment \
\n\t48465300-0000-11AA-AA11-00306543ECAC : Apple HFS/HFS+ \
\n\t55465300-0000-11AA-AA11-00306543ECAC : Apple UFS \
)

#define SUPPROTED_FILESYSTEM STR(\n\tFAT32)

int main(int argc, char *argv[]) {
    //freopen("test.tfs", "r", stdin);

    if(argc == 2 && !strcmp(argv[1], "help")) {
        puts(help_msg);
        return 0;
    }else if(argc == 1) {
        puts(help_msg);
        puts("------------------now input your command-----------------");   
    }

    bool interact_mode = false;

    std::ifstream input_file(argv[1]);
    std::istream &script = argc == 2 ? input_file : (interact_mode = true, std::cin);
    
    if(!script) {
        printf("Failed to load script %s\n", argv[1]);
        return 1;
    }

    std::map<std::string, std::string> command_help = {
        //基本
        {"open", "argument:[filename] | Set disk image file"},
        {"open_ro", "argument:[filename] | Set disk image file, readonly"},
        {"new", "argument:[filename, size_in_mb] | Create a new GPT disk image file (1mb=1024kb,1kb=1024b), No spaces in filename, pleaase"},
        {"close", "no_argument | Finish your operations and close disk image file"},
        
        //分区
        {"list_parts", "no_argument | List exist part in the image file"},
        {"new_part_start", "no_argument | Start creating new partition"},
        {"new_part_end", "no_argument | End creating new parition"},
        {"sync", "no_argument | Synchronize partition table to disk image file(Think before you do)"}, 
        {"type_guid", "argument:[guid] | Set the type guid of new partition, for example:" TYPE_GUID_REFERENCE},
        {"guid", "argument:[guid/\"random\"] | Set the guid of new partition"},
        {"start_lba", "argument:[lba] | Set start lba"},
        {"end_lba", "argument:[lba] | Set end lba"},
        {"label", "argument:[label] | Set the label"},
        {"attribute", "argument:[attr] | Set the attribute\n\t1:System partition\n\t2:EFI hidden partition\n\t4:Normal\n\t1152921504606846976:Readonly\n\t"\
        "4611686018427387904:Hidden\n\t9223372036854775808:No auto mount"},

        //选区
        {"select", "argument:[guid/index] | To specified the current partition(find the partition by guid or index)"},
        {"rmpart", "no_argument | Delete current partition. After done the current partition will come unexisting"},

        //文件系统
        {"makefs", "argument:[filesystem] | Format this partition with specified filesystem\n\tAvailable filesystem: " SUPPROTED_FILESYSTEM},
        {"enter_part", "argument:[filesystem(optional)] | Enter into the filesystem in current partition(requires fore select_part, detect filesystem automatically if no argument)"},
        {"ls", "argument:[dir(optional)] | List files in current directory"},
        {"chdir", "alias: cd | argument:[dir] | Change current directory to dir"},
        {"mkdir", "argument:[dir] | Create directory(depth = 1)"},
        {"push", "argument:[src, dest] | Send src(in real filesystem on you computer) to dest(in current directory in the image file)"},
        {"poll", "argument:[src, dest] | Poll src(in image file) to dest(in real filesystem on your computer)"}, 
        {"rm", "argument:[filename] | Remove a file or directory"},
        {"chpdir", "alias: cdp | no_argument | Change current directory to parent directory"},
        {"file", "argument:[filename] | Show detailed information about a file"},

        //杂项
        {"print", "argument:[text] | Just print the text"},
        {"help", "argument:[command(optional)] | Show help message about the command"},
        {"exit", "argument:[code(optional)] | Exit with code(default to 0)"},
        
    };

#define CHECK_FILE if(!g_fp) {printf("Error: No disk image file opened\n"); return false;}
#define CHECK_RO if(g_state.readonly) {printf("Error: Could not perform because of readonly mode\n"); return false;}
#define CHECK_PHASE(p) if(g_state.phase != p) {printf("Error: Phase check failed\n"); return false; }
#define CHECK_PTHANDLE if(!g_state.pt_handle) {printf("Error: No paritition selected\n"); return false;}
#define CHECK_FSHANDLE if(!g_state.fs_tool) {printf("Error: No active filesystem\n"); return false;}
#define CHECK_FSTOOL CHECK_FSHANDLE
    auto show_info = [&](PartInfo *info) {
        char guid_text[50];
        GUID2Text(info->guid, guid_text);
        printf("  GUID: %s\n  Label: %s\n  Description: %s\n  Start LBA: %llu | End LBA: %llu | Size: %.2lfMB\n\n", 
                guid_text, info->label, PART_DESCRIPTION[info->type], info->start_lba, info->end_lba,
                (info->end_lba-info->start_lba+1)*0.5 / 1024);
    };

    //跳过前置的空白字符读取本行剩下的所有内容
    auto remain_arg = [&](std::stringstream &s) -> std::string{
        std::string t;
        std::getline(s, t);
        const char *pname = t.c_str();
        while(isspace(*pname))
            pname++;
        return std::string(pname);
    };

    int err = STATUS_SUCCESS;
    std::map<std::string, std::function<bool(std::stringstream&, PartInfo*)>> makefs_handler = {
        {"FAT32", [&](std::stringstream &arg, PartInfo *info) -> bool {
            
            FAT32Tool fat;
            fat32_fstool_initialize(&fat, g_io, 
                info->start_lba, info->end_lba);
            err = fat.fs.makefs((FSTool*)&fat);

            return !err;
        }},
        
    };

    std::map<std::string, std::function<bool(std::stringstream&)>> command_handler = {
        {"open", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)

            if(g_fp) {
                printf("Unable to perform, plase close the file you haved opened.\n");
                return false;
            }

            std::string filename;
            std::getline(arg, filename);
            const char *pname = filename.c_str();
            while(isspace(*pname))
                pname++;
            g_fp = fopen(pname, "rb+");
            if(!g_fp) {
                printf("Failed to open file (%s)\n", pname);
                return false;
            } 
            printf("OK, Loaded (%s) as a disk image\n", pname);

            err = gpt_part_tool_initialize(&g_state.gpt_part_tool, g_io);
            if(err) return false;
            g_state.part_tool = (PartTool *)&g_state.gpt_part_tool;    //钦点GPT了，qtmd mbr
            g_state.readonly = false;

            return true;
        }},
        {"open_ro", [&](std::stringstream &arg) -> bool {  //复制粘贴大法好
            CHECK_PHASE(PHASE_FREE)

            if(g_fp) {
                printf("Unable to perform, plase close the file you haved opened.\n");
                return false;
            }

            std::string filename;
            std::getline(arg, filename);
            const char *pname = filename.c_str();
            while(isspace(*pname))
                pname++;
            g_fp = fopen(pname, "rb");  //只读
            if(!g_fp) {
                printf("Failed to open file (%s)\n", pname);
                return false;
            } 
            printf("OK, Loaded (%s) as a disk image in readonly mode\n", pname);

            gpt_part_tool_initialize(&g_state.gpt_part_tool, g_io);
            g_state.part_tool = (PartTool *)&g_state.gpt_part_tool;    //钦点GPT了，qtmd mbr
            g_state.readonly = true;

            return true;
        }}, 
        {"new", [&](std::stringstream &arg) -> bool {
            if(g_fp) {
                printf("Unable to perform, plase close the file you haved opened.\n");
                return false;
            }
            
            std::string filename;
            long long size = -1;
            arg >> filename >> size;

            FILE *newf = fopen(filename.c_str(), "wb");
            if(!newf) {
                printf("Failed to create file (%s)\n", filename.c_str());
                return false;
            } 
            if(size <= 0) {
                printf("Invalid size: %lld\n", size);
                return false;
            }
            unsigned long long size_in_byte = size * 1024 * 1024;

            FILE *old_fp = g_fp;
            g_fp = newf;
            err = gpt_part_tool_initialize(&g_state.gpt_part_tool, g_io);
            if(err) return false;
            g_state.part_tool = (PartTool *)&g_state.gpt_part_tool;    //钦点GPT了，qtmd mbr
            g_state.readonly = false;
            err = g_state.part_tool->make_part_table(g_state.part_tool, size_in_byte);
          
            if(err) {
                printf("Failed to create a disk image sized %lldmb\n", size);
            } else {
                printf("OK, Created a new disk image sized %lldmb with empty GPT table in it.\n", size);
            }
            fclose(newf);
            g_fp = old_fp;

            return !err;
        }},
        {"close", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)

            if(g_fp) {
                fclose(g_fp);
                g_fp = nullptr;
                printf("OK, Closed the disk image file\n");
            } else {
                printf("Ignored closing\n");
            }
            g_state.pt_handle = g_state.fs_handle = 0;
            return true;
        }},
        {"list_parts", [&](std::stringstream &arg) -> bool {
            CHECK_FILE            

            printf("> List parts <\n");
            FSHandle handle = 0;

            while(handle = g_state.part_tool->enum_part(g_state.part_tool, handle)) {
                PartInfo info;
                g_state.part_tool->query_part_info(g_state.part_tool, handle, &info);
                show_info(&info);
            }

            return true;
        }},
        {"new_part_start", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE) 
            CHECK_FILE
            CHECK_RO
            memset(&g_state.info, 0, sizeof(PartInfo));
            g_state.phase = PHASE_CREATE_PARTITION;
            return true;
        }},
        {"new_part_end", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_CREATE_PARTITION)
            CHECK_FILE
            
            err = g_state.part_tool->create_part(g_state.part_tool, &g_state.info, nullptr);
            if(!err) {
                printf("OK, Created a new partition:\n");
                show_info(&g_state.info);
                g_state.phase = PHASE_FREE;
                return true;
            }else return false;
        }},
        {"type_guid", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_CREATE_PARTITION) 
            CHECK_FILE
            CHECK_RO

            //Ctrl-C Ctrl-V, pname得到的是参数！
            std::string filename;
            std::getline(arg, filename);
            const char *pname = filename.c_str();
            while(isspace(*pname))
                pname++;
            
            Text2GUID(pname, &g_state.info.type_guid);
            g_state.info.type = PART_UNKNOWN;
            for(int i = 0; i < PART_UNKNOWN; i++) {
                if(!memcmp(PART_TYPE_GUID(i), g_state.info.type_guid, sizeof(GPTGUID))) {
                    g_state.info.type = PART_EMPTY + i;
                    break;
                }
            }
        
            return true;
        }},
        {"guid", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_CREATE_PARTITION) 
            CHECK_FILE
            CHECK_RO

            //Ctrl-C Ctrl-V, pname得到的是参数！
            std::string filename;
            std::getline(arg, filename);
            const char *pname = filename.c_str();
            while(isspace(*pname))
                pname++;

            if(!strncmp(pname, "random", 6))
                RandomGUID(&g_state.info.guid);
            else 
                Text2GUID(pname, &g_state.info.guid);

            return true;
        }},
        {"start_lba", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_CREATE_PARTITION) 
            CHECK_FILE
            CHECK_RO

            //Ctrl-C Ctrl-V, pname得到的是参数！
            std::string filename;
            std::getline(arg, filename);
            const char *pname = filename.c_str();
            while(isspace(*pname))
                pname++;
            
        
            sscanf(pname, "%llu", &g_state.info.start_lba);

            return true;
        }},
        {"end_lba", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_CREATE_PARTITION) 
            CHECK_FILE
            CHECK_RO
                
                //Ctrl-C Ctrl-V, pname得到的是参数！
            std::string filename;
            std::getline(arg, filename);
            const char *pname = filename.c_str();
            while(isspace(*pname))
                pname++;
        
            sscanf(pname, "%llu", &g_state.info.end_lba);

            return true;
        }},
        {"label", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_CREATE_PARTITION) 
            CHECK_FILE
            CHECK_RO

                //Ctrl-C Ctrl-V, pname得到的是参数！
            std::string filename;
            std::getline(arg, filename);
            const char *pname = filename.c_str();
            while(isspace(*pname))
                pname++;            
            strncpy(g_state.info.label, pname, 35);
            return true;
        }},
        {"attribute", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_CREATE_PARTITION) 
            CHECK_FILE
            CHECK_RO
            arg >> g_state.info.attribute;
            return true;
        }},
        {"select", [&](std::stringstream &arg) -> bool{
            CHECK_FILE

            auto s = remain_arg(arg);
            FSHandle result = 0;
            if(s.length() > 3) {
                FSHandle handle = 0;
                GPTGUID guid; 
                Text2GUID(s.c_str(), &guid);
                while(handle = g_state.part_tool->enum_part(g_state.part_tool, handle)) {
                    PartInfo info;
                    g_state.part_tool->query_part_info(g_state.part_tool, handle, &info);
                    if(!memcmp(info.guid, guid, sizeof(guid))) {
                        result = handle;
                        break;
                    }
                }
            } else {
                FSHandle handle = 0;
                int index = 0;
                sscanf(s.c_str(), "%d", &index);
                if(index < 0) {
                    err = ERROR_TFS_INVALID_ARGUMENT;
                    return false;
                }
                while(handle = g_state.part_tool->enum_part(g_state.part_tool, handle)) {
                    while(!(index--)) {
                        result = handle;
                        break;
                    }
                }
            }
            if(result) {
                PartInfo info;
                g_state.part_tool->query_part_info(g_state.part_tool, result, &info);
                char tmp[50];
                GUID2Text(info.guid, tmp);
                printf("OK, Selected the partition (GUID=%s)\n", tmp);
                g_state.pt_handle = result;
                return true;
            } else {
                printf("Error: The partition doest not exist\n");
                return false;
            }
            
        }},
        {"rmpart", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)
            CHECK_RO
            CHECK_PTHANDLE 
            
            err = g_state.part_tool->delete_part(g_state.part_tool, g_state.pt_handle);
            if(!err) {
                printf("OK, Deleted selected partition\n");
            }
            return !err;
        }},

        {"sync", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)
            CHECK_FILE
            CHECK_RO

            err = g_state.part_tool->sync_with_disk(g_state.part_tool);
            if(err) 
                return false;
            else {
                printf("OK, Synchronized specified GPT information to the disk image\n");
                return true;
            }
        }},
        //下面是文件系统工具的命令 
        {"makefs", [&](std::stringstream &arg) -> bool {
            CHECK_RO
            CHECK_PTHANDLE
            CHECK_PHASE(PHASE_FREE)

            std::string fsname;
            arg >> fsname;
            std::transform(fsname.begin(), fsname.end(), fsname.begin(), //std::toupper<char>);
                [](char c) {return std::toupper(c);});
            if(makefs_handler.find(fsname) != makefs_handler.end()) {
                PartInfo info;
                g_state.part_tool->query_part_info(g_state.part_tool, g_state.pt_handle, &info);
                if(makefs_handler[fsname](arg, &info)) {
                    char temp[50];
                    GUID2Text(info.guid, temp);
                    printf("OK, Created %s filesystem in partition (GDUI=%s)\n", fsname.c_str(), temp);
                    return true;
                }else return false;
            } else {
                printf("Error: Unknown filesystem\n");
            }

            return false;
        }}, 
        {"enter_part", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)
            CHECK_PTHANDLE

            std::string fs; 
            arg >> fs;
            if(!fs.length()) {
                printf("Error: Haven't implemented automatically detecting filesystem, please indicate it using argument.\n");
                return false; 
            } else {
                std::transform(fs.begin(), fs.end(), fs.begin(), 
                                [](char c) {return std::toupper(c);});
                PartInfo info;
                g_state.part_tool->query_part_info(g_state.part_tool, g_state.pt_handle, &info);

                if(fs == "FAT32") {
                    err = fat32_fstool_initialize(&g_state.fat32_tool, g_io, info.start_lba, info.end_lba); 
                    if(!g_state.fat32_tool.is_valid_fat32)  {
                        printf("Error: It seems not a FAT32 paritition\n");
                        return false;
                    } 
                    if(!err) {
                        g_state.fs_tool = (FSTool*)&g_state.fat32_tool;
                        g_state.fs_handle = FSTOOL_INVALID_HANDLE;
                        command_handler["_ls_silence"](arg);
                        printf("OK, entered into this part\n");
                        return true;
                    } else return false; 
                } else {
                    printf("Unknown filesystem (%s)\n", fs.c_str());
                    return false;
                }
            }

            return false;
        }},
        {"_ls_silence", [&](std::stringstream &) -> bool{
            FileHandle handle = FSTOOL_INVALID_HANDLE;
            FileInfo info;
            g_state.lookup.clear();
            while((handle = g_state.fs_tool->enum_dir(g_state.fs_tool, g_state.fs_handle, \
                                    handle, &info)) != FSTOOL_INVALID_HANDLE) {
                g_state.lookup[info.name_char] = std::pair<FileHandle, FileInfo>(handle, info);
            }
            return true;
        }},
        {"ls", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)
            CHECK_FILE
            CHECK_FSHANDLE
            printf("list directory files:\n");
            for(auto & f : g_state.lookup) {
                printf("  %s\n", f.second.second.name_char);
            }            

            return true;
        }}, 
        {"chdir", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)
            CHECK_FILE
            CHECK_FSHANDLE

            std::string dirname;
            arg >> dirname;
            if(dirname.length() == 0) {
                printf("Error: No given argument.\n");
                return false;
            }
            if(dirname == ".") {
                printf("OK\n");
                return true;
            }
            if(dirname == "..") {
                printf("Error: Please use chpdir or cdp to change to parent directory\n");
                return false;
            }            
            if(g_state.lookup.find(dirname) == g_state.lookup.end() || g_state.lookup[dirname].second.is_directory == 0) {
                printf("Error: (%s) No such directory or it's not a directory\n", dirname.c_str());
                return false;
            }

            g_state.pdir_handle = g_state.fs_handle;
            g_state.fs_handle = g_state.lookup[dirname].first;
            printf("OK, Changed current directory to ./%s\n", dirname.c_str());
            command_handler["_ls_silence"](arg);
            return true;
        }},
        {"mkdir", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)
            CHECK_FILE
            CHECK_RO

            std::string dirname;
            arg >> dirname;
            if(dirname.length() == 0) {
                err = ERROR_TFS_INVALID_ARGUMENT;
                return false;
            }
            if(g_state.lookup.find(dirname) != g_state.lookup.end()) {  //重名
                printf("Error: Repeated directory(file) name\n");
                return 0;
            }   

            FileInfo info;
            memset(&info, 0, sizeof(info));
            info.created_year = 2019;
            info.created_month = 8;
            info.created_day = 6;
            info.created_hour = 23;
            info.created_minute = 01;
            info.created_second = 1;
            info.access_year = 2019;
            info.access_month = 8;
            info.access_day = 6;
            info.access_hour = 23;
            info.access_minute = 01;
            info.access_second = 1;
            info.modify_year = 2019;
            info.modify_month = 8;
            info.modify_day = 6;
            info.modify_hour = 23;
            info.modify_minute = 01;
            info.modify_second = 1;
            info.attributes = FILE_ATTRIBUTE_NORMAL;
            info.file_size_in_bytes = 0;
            info.is_directory = 1;
            strcpy(info.name_char, dirname.c_str());

            FileHandle handle, offset = 0;
            err = g_state.fs_tool->create_file(g_state.fs_tool, g_state.fs_handle, &info, &handle);
            if(!err) {
                g_state.lookup[dirname] = std::make_pair(handle, info);
                g_state.fs_tool->sync_information(g_state.fs_tool);
                return true;
            } else {
                return false;
            }        
            
            return true;
        }},
        {"push", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)
            CHECK_FILE
            CHECK_RO
            CHECK_FSHANDLE

            std::string src, dest;
            arg >> src >> dest;
        
            if(src.length() == 0 || dest.length() == 0) {
                printf("Error: No enough argument\n");
                return false;
            }

            if(strchr(dest.c_str(), '/') || strchr(dest.c_str(), '\\')) {
                printf("Error: No directory seperator('/' or '\\' in destination filename, please\n");
                return false;
            }

            if(g_state.lookup.find(dest) != g_state.lookup.end()) {  //重名
                printf("Error: Repeated directory(file) name\n");
                return 0;
            }   

            FILE *src_file = fopen(src.c_str(), "rb");
            if(!src_file) {
                printf("Error: No such file (%s) on your computer\n", src.c_str());
            }
            
            fseek(src_file, 0, SEEK_SET);
            fseek(src_file, 0, SEEK_END);
            uint64 src_size = ftell(src_file);
            fseek(src_file, 0, SEEK_SET);
            void *src_data = malloc(src_size);
            fread(src_data, src_size, 1, src_file);
            fclose(src_file);

            //复制粘贴我很快乐！
            FileInfo info;
            memset(&info, 0, sizeof(info));
            info.created_year = 2019;
            info.created_month = 8;
            info.created_day = 6;
            info.created_hour = 23;
            info.created_minute = 01;
            info.created_second = 1;
            info.access_year = 2019;
            info.access_month = 8;
            info.access_day = 6;
            info.access_hour = 23;
            info.access_minute = 01;
            info.access_second = 1;
            info.modify_year = 2019;
            info.modify_month = 8;
            info.modify_day = 6;
            info.modify_hour = 23;
            info.modify_minute = 01;
            info.modify_second = 1;
            info.attributes = FILE_ATTRIBUTE_NORMAL;
            info.file_size_in_bytes = 0;
            strcpy(info.name_char, dest.c_str());
            FileHandle handle;
            err = g_state.fs_tool->create_file(g_state.fs_tool, g_state.fs_handle, &info, &handle);
            if(!err) {
                FileHandle offset = 0;
                uint64 size = g_state.fs_tool->fwrite(g_state.fs_tool, handle, src_data, src_size, &offset);
                info.file_size_in_bytes = size;
                g_state.lookup[dest.c_str()] = std::make_pair(handle, info);
                g_state.fs_tool->sync_information(g_state.fs_tool);
                printf("OK, Copied\n");
            } 
            free(src_data);

            return !err;
        }},
        {"poll", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)
            CHECK_FILE
            CHECK_FSHANDLE
            
            std::string src, dest;
            arg >> src >> dest;
        
            if(src.length() == 0 || dest.length() == 0) {
                printf("Error: No enough argument\n");
                return false;
            }

            if(strchr(src.c_str(), '/') || strchr(src.c_str(), '\\')) {
                printf("Error: No directory seperator('/' or '\\' in source filename, please\n");
                return false;
            }

            if(g_state.lookup.find(src) == g_state.lookup.end()) {
                printf("Error: No such file or directory :(%s)\n", src.c_str());
                return false;
            }

            //lookup表中有大小信息，但是可能是过时的，重新查询一下
            auto &info = g_state.lookup[src];
            if(err = g_state.fs_tool->query_file_info(g_state.fs_tool, info.first, &info.second)) {
                return false;
            }
            
            FILE *fout = fopen(dest.c_str(), "wb");
            if(!fout) {
                printf("Error: Unable to create (%s) on your computer\n", dest.c_str());
                return false;
            }

            uint64 filesize = info.second.file_size_in_bytes;
            void *buffer = malloc(filesize);
            FileHandle offset = 0;
            g_state.fs_tool->fread(g_state.fs_tool, info.first, buffer, filesize, &offset);

            fwrite(buffer, filesize, 1, fout);
            fclose(fout);
            free(buffer);
            printf("OK, Copied\n");

            return true;
        }},
        {"rm", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)
            CHECK_FILE
            CHECK_RO

            std::string name;
            arg >> name;
            if(name.length() == 0) {
                err = ERROR_TFS_INVALID_ARGUMENT;
                return false;
            }
            if(g_state.lookup.find(name) == g_state.lookup.end()) {  //???
                printf("Error: No such file or directory: (%s)\n", name.c_str());
                return false;
            }   

            g_state.fs_tool->delete_file(g_state.fs_tool, g_state.lookup[name].first);
            g_state.lookup.erase(name);
            g_state.fs_tool->sync_information(g_state.fs_tool);
            printf("OK, Deleted (%s)\n", name.c_str());

            return true;
        }},
        {"chpdir", [&](std::stringstream &arg) -> bool {
            CHECK_PHASE(PHASE_FREE)
            CHECK_FILE
            CHECK_FSHANDLE

            g_state.fs_handle = g_state.pdir_handle;
            command_handler["_ls_silence"](arg);
            printf("OK, Changed current directory to parent directory\n");

            return true;
        }},

        //别名
        {"cd", [&](std::stringstream &arg) -> bool {
            return command_handler["chdir"](arg);
        }},

        {"cdp", [&](std::stringstream &arg) -> bool {
            return command_handler["chpdir"](arg);
        }},

        //下面是其它命令
        {"print", [&](std::stringstream &arg) -> bool {
            //复制粘贴大法好
            std::string filename;
            std::getline(arg, filename);
            const char *pname = filename.c_str();
            while(isspace(*pname))
                pname++;
            puts(pname);
            return true;
        }},
        {"help", [&](std::stringstream &arg) -> bool {
            std::string s;
            arg >> s;
            if(s.length()) {
                puts(command_help[s].c_str());
            } else {
                for(auto &c : command_help) {
                    printf("%s: %s\n", c.first.c_str(), c.second.c_str());
                }
            }
            return true;
        }},
        {"exit", [&](std::stringstream &arg) -> bool {
            int exit_code = 0;
            if(arg) arg >> exit_code;
            if(g_fp)
                fclose(g_fp);
            exit(exit_code);
            return true;
        }},
        //自定义的test区域
        {"test", [&](std::stringstream &arg) -> bool {
            FileInfo info;
            memset(&info, 0, sizeof(info));
            info.created_year = 2019;
            info.created_month = 8;
            info.created_day = 6;
            info.created_hour = 23;
            info.created_minute = 01;
            info.created_second = 1;
            info.access_year = 2019;
            info.access_month = 8;
            info.access_day = 6;
            info.access_hour = 23;
            info.access_minute = 01;
            info.access_second = 1;
            info.modify_year = 2019;
            info.modify_month = 8;
            info.modify_day = 6;
            info.modify_hour = 23;
            info.modify_minute = 01;
            info.modify_second = 1;
            info.attributes = FILE_ATTRIBUTE_NORMAL;
            info.file_size_in_bytes = 0;
            strcpy(info.name_char, "ABC.TXT");
            FileHandle handle, offset = 0;
            g_state.fs_tool->create_file(g_state.fs_tool, FSTOOL_INVALID_HANDLE, &info, &handle);
            g_state.fs_tool->fwrite(g_state.fs_tool, handle, "SXYISGOOD\n", 10, &offset);

            //dir hhh
            info.is_directory = 1;
            strcpy(info.name_char, "HHH");
            g_state.fs_tool->create_file(g_state.fs_tool, FSTOOL_INVALID_HANDLE, &info, &handle);
            g_state.fs_tool->sync_information(g_state.fs_tool);
            command_handler["_ls_silence"](arg);
            return true;
        }}
    };

    g_state.phase = PHASE_FREE;
    g_state.fs_handle = g_state.pdir_handle = FSTOOL_INVALID_HANDLE;
    int line_number = 1;
    std::string line; 
    if(interact_mode)
        printf(">>>");
    while(std::getline(script, line)) {
        err = STATUS_SUCCESS;
        auto commands = std::stringstream(line);
        if(line[0] == '#') {
            line_number++;
            continue;  //跳过注释行
        }
        
        std::string cmd; 
        commands >> cmd;

        if(cmd.length() == 0) {
            line_number++;
            continue;  //跳过空行
        }
        
        bool ok = true;

        if(command_handler.find(cmd) == command_handler.end()) {     
            printf("Unknown command: %s\n", cmd.c_str());
            ok = false;    
        } else {
            ok = command_handler[cmd](commands);
        }

        if(!ok) {
            if(err) {
                printf("Error: %s\n", ERROR_TFS_STRING[err - ERROR_TFS_START]);
            }
            if(!interact_mode) {  //对于脚本文件，中断执行
                printf("Abort");
                printf(" at script file: %s, line: %d, command: %s\n", argv[1], line_number, cmd.c_str());
                if(g_fp)
                    fclose(g_fp);
                return 1;
            }
        }
        if(interact_mode)
            printf(">>>");

        line_number++;
    }   
    
    if(g_fp)
        fclose(g_fp);
    return 0; 
}
