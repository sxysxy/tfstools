# tfstools
tfstools是一个交互式/脚本化操作的，核心库完全由C语言写成，易于移植的GPT分区与文件系统工具。

最初是作为<a href="https://github.com/sxysxy/Taurix">Taurix</a>操作系统内核的最基本的文件系统的实现，由于核心的库的高度可移植性（核心库完全使用C语言及其标准库，整个工具只使用C/C++语言及其标准库），现将tfstools单独发布。

# 特性
<table>
<th> <th> 自识别 <th> 创建 <th> 读取 <th> 写入<br>
<tr>
<td align="center"> GPT分区 <td align="center" >√<td align="center">√<td align="center">√ <td align="center">√<br>
</tr>
<tr>
<td align="center"> FAT32 <td align="center">待完善<td align="center">√<td align="center">√<td align="center">√ <br>
</tr>
<tr>
</tr>
</table>
更多文件系统的支持正在开发中

# 构建工具
## *nix
```
git clone git@github.com:sxysxy/tfstools
cd tfstools
#构建核心库：
./makelib.sh
#构建工具
./maketool.sh
#安装到/usr/bin下
sudo ./install.sh
```
## Windows:
下载源代码，将所有源代码一起使用g++或cl.exe编译即可生成工具

除去tfstools.cpp，可以使用gcc或cl.exe编译成静态库或动态链接库

# 核心库API

见tfstools.h，以及工具前端tfstools.cpp的用例

# 命令

## 常规

<table>
<th> 命令 <th> 参数 <th> 说明 <br>
<tr><td> new <td> [filename, size_in_mb] <td>  Create a new GPT disk image file (1mb=1024kb,1kb=1024b), No spaces in filename, pleaase
</tr>
<tr>
<td> open <td> [filename] <td> Set disk image file
</tr>
<tr>
<td> open_ro <td> [filename] <td> Set disk image file, readonly
</tr>
<tr>
<td>close<td>  <td> Finish your operations and close disk image file
</tr>
<tr>
<td>exit<td> [code(optional)] <td> Exit with code(default to 0)
</tr>
<tr>
<td>help <td> [command(optional)] <td> Show help message about the command
</tr>
<tr>
<td>print<td>[text] <td> Just print the text
</tr>
</table>

## 分区
<table>
<th>命令<th>参数<th>说明
<tr>
<td>list_parts <td> <td> List exist part in the image file
</tr>
<td>new_part_start <td> <td> Start creating new partition
<tr>
<td> attribute <td> [attr] <td> <pre> Set the attribute
        1                   :System partition
        2                   :EFI hidden partition
        4                   :Normal
        1152921504606846976 :Readonly
        4611686018427387904 :Hidden
        9223372036854775808 :No auto mount </pre>
</tr>
<tr>
<td> type_guid <td> [guid] <td> <pre> Set the type guid of new partition, for example:
        C12A7328-F81F-11D2-BA4B-00A0C93EC93B : EFI System 
        EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 : Data 
        0657FD6D-A4AB-43C4-84E5-0933C84B4F4F : Linux Swapfile 
        DE94BBA4-06D1-4D40-A16A-BFD50179D6AC : Windows Recovery Environment 
        48465300-0000-11AA-AA11-00306543ECAC : Apple HFS/HFS+ 
        55465300-0000-11AA-AA11-00306543ECAC : Apple UFS </pre>
</tr>

<tr>

<td>guid <td> [guid/"random"] <td> Set the guid of new partition, or use random 
</tr>
<td>start_lba <td> [lba] <td> Set start lba
<tr>
<td>end_lba <td> [lba] <td> Set end lba
</tr>

<tr>
<td>label <td> [label] <td> Set the label
</tr>

<tr>
<td>new_part_end <td> <td> End creating new parition
</tr>

<tr>
<td>select <td>[guid/index] <td> To specified the current partition(find the partition by guid or index)
</tr>

<tr>
<td>rmpart <td> <td> Delete current partition. After done the current partition will come unexisting

<tr>
<td>
enter_part <td> [filesystem(optional)] <td> Enter into the filesystem in current partition(requires fore select_part, detect filesystem automatically if no argument)
</tr>

<tr>
<td>sync <td> <td> Synchronize partition table to disk image file(Think before you do)

</tr>
</table>

## 文件系统

<table>
<th> 命令(简写) <th> 参数 <th> 说明

<tr>
<td> makefs <td> [filesystem] <td> <pre> Format this partition with specified filesystem
   Available filesystem: 
        FAT32 </pre>
</tr>

<tr>
<td> chdir(cd) <td> [dir] <td> Change current directory to dir
</tr>
<tr>
<td>chpdir(cdp)<td> <td>Change current directory to parent directory
</tr>

<tr>
<td>mkdir <td>[dir] <td> Create directory(depth = 1)
</tr>

<tr>
<td>ls <td> [dir(optional)] <td> List files in current directory
</tr>

<tr>
<td>poll <td> [src, dest] <td> Poll src(in image file) to dest(in real filesystem on your computer)
</tr>

<tr>
<td>push <td> [src, dest] <td> Send src(in real filesystem on you computer) to dest(in current directory in the image file)
</tr>

<tr>
<td>rm <td> [filename] <td> Remove a file or directory
</table>

用例见<a href="https://github.com/sxysxy/tfstools/blob/master/demo.tfs">demo.tfs</a>
