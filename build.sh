#!/bin/bash

# Moss 内核多架构构建脚本
# 编译所有或指定的 CMake workflow 预设

set -e  # 遇到错误时退出

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# 构建统计
BUILD_SUCCESS=0
BUILD_FAILED=0
BUILD_SKIPPED=0
START_TIME=$(date +%s)

# 构建结果存储
declare -A BUILD_RESULTS
declare -A BUILD_TIMES
declare -a FAILED_BUILDS
declare -a SUCCESS_BUILDS
declare -a SKIPPED_BUILDS

# 显示帮助信息
show_help() {
    echo -e "${BOLD}Moss 内核多架构构建脚本${NC}"
    echo
    echo "用法: $0 [选项]"
    echo
    echo "选项:"
    echo "  -h, --help           显示此帮助信息"
    echo "  -l, --list           列出所有可用的 workflow 预设"
    echo "  -a, --all            构建所有架构 (默认)"
    echo "  -m, --main           只构建主要架构 (ARM64, x86_64)"
    echo "  -d, --debug          只构建 debug 版本"
    echo "  -r, --release        只构建 release 版本"
    echo "  -c, --clean          构建前清理所有构建目录"
    echo "  -j, --jobs N         指定并行作业数 (默认: $(nproc))"
    echo "  -v, --verbose        显示详细构建输出"
    echo "  --arch ARCH          只构建指定架构 (arm64, x86_64, riscv)"
    echo "  --preset PRESET      只构建指定的预设"
    echo "  --dry-run            只显示将要执行的命令，不实际构建"
    echo
    echo "示例:"
    echo "  $0 -a                # 构建所有架构"
    echo "  $0 -m -d             # 只构建主要架构的 debug 版本"
    echo "  $0 --arch arm64      # 只构建 ARM64 架构"
    echo "  $0 --preset debug    # 只构建默认 debug 预设"
}

# 列出所有可用的 workflow 预设
list_presets() {
    echo -e "${BOLD}可用的 Workflow 预设:${NC}"
    echo
    cmake --list-presets workflow 2>/dev/null | grep -E '^\s+".*"' | while read -r line; do
        preset=$(echo "$line" | sed 's/.*"\([^"]*\)".*/\1/')
        description=$(echo "$line" | sed 's/.*" - \(.*\)/\1/')
        echo -e "  ${CYAN}$preset${NC} - $description"
    done
}

# 获取所有 workflow 预设
get_workflow_presets() {
    cmake --list-presets workflow 2>/dev/null | grep -E '^\s+".*"' | sed 's/.*"\([^"]*\)".*/\1/' || true
}

# 打印带时间戳的消息
log_message() {
    local level=$1
    shift
    local message="$*"
    local timestamp=$(date '+%H:%M:%S')

    case $level in
        "INFO")
            echo -e "${CYAN}[$timestamp]${NC} $message"
            ;;
        "SUCCESS")
            echo -e "${GREEN}[$timestamp]${NC} ✓ $message"
            ;;
        "ERROR")
            echo -e "${RED}[$timestamp]${NC} ✗ $message"
            ;;
        "WARNING")
            echo -e "${YELLOW}[$timestamp]${NC} ⚠ $message"
            ;;
        "SKIP")
            echo -e "${YELLOW}[$timestamp]${NC} ⏭ $message"
            ;;
    esac
}

# 执行构建命令
execute_build() {
    local preset=$1
    local build_start=$(date +%s)

    log_message "INFO" "开始构建预设: ${BOLD}$preset${NC}"

    if [[ "$DRY_RUN" == "true" ]]; then
        echo "  [DRY-RUN] cmake --workflow --preset $preset"
        BUILD_RESULTS[$preset]="DRY-RUN"
        BUILD_SKIPPED=$((BUILD_SKIPPED + 1))
        SKIPPED_BUILDS+=("$preset")
        return 0
    fi

    local build_log="/tmp/moss_build_${preset}_$(date +%s).log"
    local build_cmd="cmake --workflow --preset $preset"

    if [[ "$VERBOSE" == "true" ]]; then
        echo -e "${BLUE}执行命令: $build_cmd${NC}"
        if $build_cmd 2>&1 | tee "$build_log"; then
            local build_end=$(date +%s)
            local build_time=$((build_end - build_start))
            BUILD_TIMES[$preset]=$build_time
            BUILD_RESULTS[$preset]="SUCCESS"
            BUILD_SUCCESS=$((BUILD_SUCCESS + 1))
            SUCCESS_BUILDS+=("$preset")
            log_message "SUCCESS" "构建成功: $preset (${build_time}s)"
        else
            local build_end=$(date +%s)
            local build_time=$((build_end - build_start))
            BUILD_TIMES[$preset]=$build_time
            BUILD_RESULTS[$preset]="FAILED"
            BUILD_FAILED=$((BUILD_FAILED + 1))
            FAILED_BUILDS+=("$preset")
            log_message "ERROR" "构建失败: $preset (${build_time}s)"
            log_message "ERROR" "构建日志: $build_log"
        fi
    else
        if $build_cmd >"$build_log" 2>&1; then
            local build_end=$(date +%s)
            local build_time=$((build_end - build_start))
            BUILD_TIMES[$preset]=$build_time
            BUILD_RESULTS[$preset]="SUCCESS"
            BUILD_SUCCESS=$((BUILD_SUCCESS + 1))
            SUCCESS_BUILDS+=("$preset")
            log_message "SUCCESS" "构建成功: $preset (${build_time}s)"
            rm -f "$build_log"  # 成功时删除日志文件
        else
            local build_end=$(date +%s)
            local build_time=$((build_end - build_start))
            BUILD_TIMES[$preset]=$build_time
            BUILD_RESULTS[$preset]="FAILED"
            BUILD_FAILED=$((BUILD_FAILED + 1))
            FAILED_BUILDS+=("$preset")
            log_message "ERROR" "构建失败: $preset (${build_time}s)"
            log_message "ERROR" "查看详细日志: $build_log"
        fi
    fi
}

# 清理构建目录
clean_builds() {
    log_message "INFO" "清理所有构建目录..."
    if [[ "$DRY_RUN" == "true" ]]; then
        echo "  [DRY-RUN] rm -rf build/ install/"
    else
        rm -rf build/ install/
        log_message "SUCCESS" "构建目录已清理"
    fi
}

# 过滤预设
filter_presets() {
    local presets=("$@")
    local filtered_presets=()

    for preset in "${presets[@]}"; do
        local should_include=true

        # 架构过滤
        if [[ -n "$TARGET_ARCH" ]]; then
            case "$TARGET_ARCH" in
                "arm64")
                    if [[ ! "$preset" =~ arm64 ]] && [[ "$preset" != "debug" ]] && [[ "$preset" != "release" ]] && [[ "$preset" != "debug-test" ]] && [[ "$preset" != "debug-gdb" ]]; then
                        should_include=false
                    fi
                    ;;
                "x86_64")
                    if [[ ! "$preset" =~ x86_64 ]]; then
                        should_include=false
                    fi
                    ;;
                "riscv")
                    if [[ ! "$preset" =~ riscv ]]; then
                        should_include=false
                    fi
                    ;;
            esac
        fi

        # 构建类型过滤
        if [[ "$BUILD_TYPE" == "debug" ]] && [[ ! "$preset" =~ debug ]]; then
            should_include=false
        elif [[ "$BUILD_TYPE" == "release" ]] && [[ ! "$preset" =~ release ]]; then
            should_include=false
        fi

        # 主要架构过滤
        if [[ "$MAIN_ONLY" == "true" ]]; then
            if [[ ! "$preset" =~ arm64 ]] && [[ ! "$preset" =~ x86_64 ]] && [[ "$preset" != "debug" ]] && [[ "$preset" != "release" ]]; then
                should_include=false
            fi
        fi

        # 特定预设过滤
        if [[ -n "$SPECIFIC_PRESET" ]] && [[ "$preset" != "$SPECIFIC_PRESET" ]]; then
            should_include=false
        fi

        if [[ "$should_include" == "true" ]]; then
            filtered_presets+=("$preset")
        fi
    done

    echo "${filtered_presets[@]}"
}

# 生成构建报告
generate_report() {
    local total_time=$(($(date +%s) - START_TIME))

    echo
    echo -e "${BOLD}==================== 构建报告 ====================${NC}"
    echo -e "总构建时间: ${CYAN}${total_time}s${NC}"
    echo -e "成功构建: ${GREEN}$BUILD_SUCCESS${NC}"
    echo -e "失败构建: ${RED}$BUILD_FAILED${NC}"
    echo -e "跳过构建: ${YELLOW}$BUILD_SKIPPED${NC}"
    echo

    if [[ ${#SUCCESS_BUILDS[@]} -gt 0 ]]; then
        echo -e "${GREEN}✓ 成功构建的预设:${NC}"
        for preset in "${SUCCESS_BUILDS[@]}"; do
            local time=${BUILD_TIMES[$preset]:-0}
            echo -e "  ${GREEN}$preset${NC} (${time}s)"
        done
        echo
    fi

    if [[ ${#FAILED_BUILDS[@]} -gt 0 ]]; then
        echo -e "${RED}✗ 失败构建的预设:${NC}"
        for preset in "${FAILED_BUILDS[@]}"; do
            local time=${BUILD_TIMES[$preset]:-0}
            echo -e "  ${RED}$preset${NC} (${time}s)"
        done
        echo
    fi

    if [[ ${#SKIPPED_BUILDS[@]} -gt 0 ]]; then
        echo -e "${YELLOW}⏭ 跳过构建的预设:${NC}"
        for preset in "${SKIPPED_BUILDS[@]}"; do
            echo -e "  ${YELLOW}$preset${NC}"
        done
        echo
    fi

    # 构建结果目录信息
    if [[ -d "build" ]]; then
        echo -e "${CYAN}构建结果目录:${NC}"
        du -sh build/*/ 2>/dev/null | while read size dir; do
            echo "  $size - $dir"
        done
        echo
    fi

    echo -e "${BOLD}================================================${NC}"

    # 返回适当的退出码
    if [[ $BUILD_FAILED -gt 0 ]]; then
        exit 1
    fi
}

# 主函数
main() {
    # 默认参数
    local BUILD_ALL=true
    local CLEAN_FIRST=false
    local JOBS=$(nproc)
    BUILD_TYPE=""
    MAIN_ONLY=false
    TARGET_ARCH=""
    SPECIFIC_PRESET=""
    VERBOSE=false
    DRY_RUN=false

    # 解析命令行参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -l|--list)
                list_presets
                exit 0
                ;;
            -a|--all)
                BUILD_ALL=true
                shift
                ;;
            -m|--main)
                MAIN_ONLY=true
                BUILD_ALL=false
                shift
                ;;
            -d|--debug)
                BUILD_TYPE="debug"
                shift
                ;;
            -r|--release)
                BUILD_TYPE="release"
                shift
                ;;
            -c|--clean)
                CLEAN_FIRST=true
                shift
                ;;
            -j|--jobs)
                JOBS="$2"
                shift 2
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            --arch)
                TARGET_ARCH="$2"
                shift 2
                ;;
            --preset)
                SPECIFIC_PRESET="$2"
                shift 2
                ;;
            --dry-run)
                DRY_RUN=true
                shift
                ;;
            *)
                echo -e "${RED}未知选项: $1${NC}"
                show_help
                exit 1
                ;;
        esac
    done

    # 显示构建配置
    echo -e "${BOLD}Moss 内核多架构构建脚本${NC}"
    echo -e "构建时间: ${CYAN}$(date)${NC}"
    echo -e "并行作业: ${CYAN}$JOBS${NC}"
    [[ -n "$TARGET_ARCH" ]] && echo -e "目标架构: ${CYAN}$TARGET_ARCH${NC}"
    [[ -n "$BUILD_TYPE" ]] && echo -e "构建类型: ${CYAN}$BUILD_TYPE${NC}"
    [[ "$MAIN_ONLY" == "true" ]] && echo -e "构建范围: ${CYAN}主要架构${NC}"
    [[ "$DRY_RUN" == "true" ]] && echo -e "${YELLOW}模式: DRY-RUN${NC}"
    echo

    # 清理构建目录
    if [[ "$CLEAN_FIRST" == "true" ]]; then
        clean_builds
        echo
    fi

    # 获取所有可用的 workflow 预设
    local all_presets=($(get_workflow_presets))
    if [[ ${#all_presets[@]} -eq 0 ]]; then
        log_message "ERROR" "未找到任何 workflow 预设"
        exit 1
    fi

    # 过滤预设
    local target_presets=($(filter_presets "${all_presets[@]}"))
    if [[ ${#target_presets[@]} -eq 0 ]]; then
        log_message "ERROR" "没有匹配的预设可构建"
        exit 1
    fi

    log_message "INFO" "将要构建 ${#target_presets[@]} 个预设: ${target_presets[*]}"
    echo

    # 执行构建
    for preset in "${target_presets[@]}"; do
        execute_build "$preset"
        echo
    done

    # 生成报告
    generate_report
}

# 检查依赖
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}错误: 未找到 cmake 命令${NC}"
    exit 1
fi

# 检查是否在正确的目录
if [[ ! -f "CMakeLists.txt" ]] || [[ ! -f "CMakePresets.json" ]]; then
    echo -e "${RED}错误: 请在项目根目录运行此脚本${NC}"
    exit 1
fi

# 运行主函数
main "$@"