# 先整体概括这份 CMake 代码作用
这段 CMake 功能：**自动扫描项目下所有 `.proto` 文件，调用 protoc 生成 C++ 的 `.pb.h` / `.pb.cc`，然后把生成的源码打包成一个静态库 `minicyber_proto`，业务代码直接链接这个库就能使用 protobuf。**

## 假设项目目录结构
```
your_project/
├── CMakeLists.txt      # 就是这段脚本
└── include/
    └── minicyber/
        └── proto/
            └── msg/
                └── test.proto   # 我们拿这个作为示例PROTO_FILE
```
路径完整：
`${CMAKE_CURRENT_SOURCE_DIR}/include/minicyber/proto/msg/test.proto`

> 变量小科普：
> `${CMAKE_CURRENT_SOURCE_DIR}`：当前 CMakeLists.txt 所在源码目录（源代码文件夹，不会变）
> `${CMAKE_CURRENT_BINARY_DIR}`：build 构建输出目录（编译产生文件放这里）

---

# 逐段讲解 + 代入示例跟踪变量

## 1. 查找系统 Protobuf
```cmake
find_package(Protobuf REQUIRED)
```
- **CMake 语法**：`find_package(<包名> [REQUIRED])`
- 作用：在系统中寻找 protobuf，找到后自动定义一堆内置变量：
  - `Protobuf_INCLUDE_DIRS`：protobuf 头文件路径
  - `Protobuf_LIBRARIES`：protobuf 库（libprotobuf.so / protobuf.lib）
  - `Protobuf_PROTOC_EXECUTABLE`：protoc 编译器程序路径
- `REQUIRED`：找不到 protobuf **直接终止编译，报错退出**，不会继续构建。

```cmake
include_directories(${Protobuf_INCLUDE_DIRS})
```
- `include_directories(路径)`：全局添加头文件搜索目录（旧风格CMake）

## 2. 设置代码生成输出目录
```cmake
set(PROTO_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
```
- CMake语法：`set(变量名 值)`，定义自定义变量
- 示例展开：
假设 build 目录是 `your_project/build`
=> `PROTO_BINARY_DIR = your_project/build/proto_gen`

## 3. 递归搜集全部 proto 文件
```cmake
file(GLOB_RECURSE PROTO_FILES "include/minicyber/proto/*.proto")
```
CMake 文件操作语法：
- `file(GLOB_RECURSE 变量 "匹配路径")`
- `GLOB_RECURSE`：递归遍历目录，匹配通配符文件
结果：
`PROTO_FILES` 是一个**列表**，里面存放所有匹配的 `.proto` 绝对路径。
我们示例中列表内容：
```
[ "/xxx/your_project/include/minicyber/proto/msg/test.proto" ]
```

> 注意：列表变量后续可以用 `foreach` 遍历、`list(APPEND)` 添加元素。

## 4. foreach 循环逐个处理 proto【核心重点】
```cmake
foreach(PROTO_FILE ${PROTO_FILES})
    # 循环体内部，每一轮 PROTO_FILE 等于列表中一个proto文件路径
endforeach()
```
CMake循环语法：
```cmake
foreach(迭代变量 ${列表变量})
  ...
endforeach()
```
我们第一轮循环：
```
PROTO_FILE = /xxx/your_project/include/minicyber/proto/msg/test.proto
```

### 4.1 file(RELATIVE_PATH) 计算相对路径
```cmake
file(RELATIVE_PATH PROTO_REL 
    ${CMAKE_CURRENT_SOURCE_DIR}/include 
    ${PROTO_FILE})
```
语法：`file(RELATIVE_PATH 输出变量 基准目录 目标文件)`
含义：算出「目标文件」相对于「基准目录」的相对路径

代入示例：
基准目录：`/xxx/your_project/include`
目标文件：`/xxx/your_project/include/minicyber/proto/msg/test.proto`
得到：
```cmake
PROTO_REL = minicyber/proto/msg/test.proto
```

### 4.2 get_filename_component 提取目录、文件名
```cmake
get_filename_component(PROTO_DIR ${PROTO_REL} DIRECTORY)
```
- `DIRECTORY`：取出路径中的文件夹部分
`PROTO_REL=minicyber/proto/msg/test.proto`
=> `PROTO_DIR = minicyber/proto/msg`

```cmake
get_filename_component(PROTO_NAME ${PROTO_REL} NAME_WE)
```
- `NAME_WE` = Name Without Extension：不带后缀文件名
=> `PROTO_NAME = test`

### 4.3 list(APPEND) 收集生成的源文件
```cmake
list(APPEND PROTO_SRCS ${PROTO_BINARY_DIR}/${PROTO_DIR}/${PROTO_NAME}.pb.cc)
```
CMake列表操作：`list(APPEND 列表变量 元素)`，往列表追加一条字符串

代入拼接：
```
PROTO_BINARY_DIR = your_project/build/proto_gen
PROTO_DIR        = minicyber/proto/msg
PROTO_NAME       = test
=>
your_project/build/proto_gen/minicyber/proto/msg/test.pb.cc
```
这条路径存入 `PROTO_SRCS` 列表。
最终 `PROTO_SRCS` 保存**所有待编译的pb.cc文件**。

### 4.4 add_custom_command：自定义命令，执行protoc【重中之重】
```cmake
add_custom_command(
    OUTPUT 输出文件1 输出文件2
    COMMAND 要执行的命令
    DEPENDS 依赖文件
    COMMENT 打印提示信息
)
```
CMake核心规则：
`add_custom_command` 定义**文件生成规则**：
> 如果 OUTPUT 指定的文件不存在 / DEPENDS 文件被修改 → 自动执行 COMMAND

```cmake
OUTPUT ${PROTO_BINARY_DIR}/${PROTO_DIR}/${PROTO_NAME}.pb.cc
       ${PROTO_BINARY_DIR}/${PROTO_DIR}/${PROTO_NAME}.pb.h
```
代表：这条命令会产出这两个文件（生成的源码+头文件）

```cmake
COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_BINARY_DIR}
```
调用cmake内置命令创建文件夹，防止目录不存在protoc报错。

```cmake
COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    --cpp_out=${PROTO_BINARY_DIR}
    -I ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROTO_FILE}
```
等价shell命令（方便你理解）：
```bash
protoc \
  --cpp_out=your_project/build/proto_gen \
  -I your_project/include \
  your_project/include/minicyber/proto/msg/test.proto
```
- `-I`：proto import搜索路径（对应proto里 `import "minicyber/xxx.proto";`）
- `--cpp_out`：C++代码输出根目录

运行后生成：
```
your_project/build/proto_gen/minicyber/proto/msg/test.pb.h
your_project/build/proto_gen/minicyber/proto/msg/test.pb.cc
```

```cmake
DEPENDS ${PROTO_FILE}
```
依赖当前proto文件；只要test.proto改动，cmake就会重新执行protoc生成代码。

## 5. 添加生成目录到头文件路径
```cmake
include_directories(${PROTO_BINARY_DIR})
```
这样业务代码可以直接：
```cpp
#include "minicyber/proto/msg/test.pb.h"
```

## 6. 把所有生成pb.cc打包成静态库
```cmake
add_library(minicyber_proto STATIC ${PROTO_SRCS})
```
语法：`add_library(库名 STATIC/SHARED 源码列表)`
- `STATIC`：静态库 `.a` / `.lib`
- 源码列表就是前面循环收集的所有 `.pb.cc`

```cmake
target_link_libraries(minicyber_proto ${Protobuf_LIBRARIES})
```
静态库 `minicyber_proto` 需要链接系统的protobuf底层库。

```cmake
target_include_directories(minicyber_proto
    PUBLIC ${PROTO_BINARY_DIR}
    PUBLIC ${Protobuf_INCLUDE_DIRS}
)
```
- `PUBLIC` 属性：任何链接 `minicyber_proto` 的其他可执行文件/库，**自动继承这两个头文件目录**
好处：上层业务只需要写：
```cmake
target_link_libraries(app minicyber_proto)
```
不用再重复加 protobuf 头文件路径，接口更干净。

---

# 完整流程时序总结（用 test.proto 这条线串起来）
1. CMake启动，找到protoc、protobuf库
2. 搜索得到 `test.proto`
3. 计算相对路径 `minicyber/proto/msg/test.proto`
4. 拆分目录、文件名
5. 注册一条命令：运行protoc，在build/proto_gen下生成 test.pb.h / test.pb.cc
6. 将生成的 .pb.cc 存入 PROTO_SRCS
7. 循环结束，所有proto都注册完生成规则
8. 创建静态库 minicyber_proto，编译全部 *.pb.cc
9. 库链接系统protobuf，并向外暴露头文件路径
10. 上层程序 link minicyber_proto，直接使用proto生成代码

# 常见坑提醒（对应这份脚本）
1. `file(GLOB)` 在cmake配置阶段扫描文件；新增proto后需要**重新执行cmake**才会被发现；
2. `-I` 参数路径一定要和源码include目录对齐，否则proto内部import会报错；
3. `add_custom_command` 不会立刻执行，构建阶段（make/build）才运行protoc。
