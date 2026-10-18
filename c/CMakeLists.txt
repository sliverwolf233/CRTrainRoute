# CRTrainRoute C 版构建（规范见 c/PLAN.md §8）
# 目标：crtr_core（静态库）、crtr_core_test；c/server/*.c 由 parallel-c
# 提供，存在时追加构建 crtr_server 与 crtr_query，缺省不影响核心构建。
cmake_minimum_required(VERSION 3.15)
project(crtr C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

# ---- 核心库：c/core/*.c（剔除自测） ----
file(GLOB CRTR_CORE_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/core/*.c")
list(FILTER CRTR_CORE_SRC EXCLUDE REGEX "core_test\\.c$")

add_library(crtr_core STATIC ${CRTR_CORE_SRC})
target_include_directories(crtr_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core")
if(NOT WIN32)
    target_link_libraries(crtr_core PUBLIC m)
endif()

# ---- 核心自测 ----
add_executable(crtr_core_test core/core_test.c)
target_link_libraries(crtr_core_test PRIVATE crtr_core)

enable_testing()
add_test(NAME core_test COMMAND crtr_core_test)

# ---- 服务端（parallel-c 的 c/server/*.c，按入口文件存在性门控） ----
file(GLOB CRTR_SERVER_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/server/*.c")
if(CRTR_SERVER_SRC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server/main.c")
    # crtr_server = server/*.c 去掉 query_main.c
    list(FILTER CRTR_SERVER_SRC EXCLUDE REGEX "query_main\\.c$")
    add_executable(crtr_server ${CRTR_SERVER_SRC})
    target_link_libraries(crtr_server PRIVATE crtr_core)
    if(WIN32)
        target_link_libraries(crtr_server PRIVATE ws2_32)
    else()
        target_link_libraries(crtr_server PRIVATE m)
    endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server/query_main.c")
    # crtr_query = server/*.c 去掉 main.c（CLI 单查询入口）
    file(GLOB CRTR_QUERY_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/server/*.c")
    list(FILTER CRTR_QUERY_SRC EXCLUDE REGEX "/main\\.c$")
    add_executable(crtr_query ${CRTR_QUERY_SRC})
    target_link_libraries(crtr_query PRIVATE crtr_core)
    if(WIN32)
        target_link_libraries(crtr_query PRIVATE ws2_32)
    else()
        target_link_libraries(crtr_query PRIVATE m)
    endif()
endif()
