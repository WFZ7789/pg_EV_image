#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
    cat <<'EOF'
用法：
  ./download_cloud.sh [输出目录]
      从标准输入逐行读取下载链接，按 Ctrl-D 结束输入。
      未指定输出目录时使用 ./download。

  ./download_cloud.sh 链接列表文件 输出目录
      兼容原来的批量下载用法。

  ./download_cloud.sh -h | --help
EOF
}

die() {
    printf '错误：%s\n' "$*" >&2
    exit 2
}

trim() {
    local value=$1
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "$value"
}

if ! command -v wget >/dev/null 2>&1; then
    die '找不到 wget，请先安装 wget。'
fi

output_dir='./download'
input_file=''

case $# in
    0)
        ;;
    1)
        case $1 in
            -h|--help)
                usage
                exit 0
                ;;
        esac

        # 一个参数既可以是旧式的链接列表文件，也可以是新的输出目录。
        if [[ -f $1 ]]; then
            input_file=$1
        else
            output_dir=$1
        fi
        ;;
    2)
        input_file=$1
        output_dir=$2
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

if [[ -n $input_file && $input_file != '-' && ! -r $input_file ]]; then
    die "无法读取链接列表文件：$input_file"
fi

mkdir -p "$output_dir" || die "无法创建输出目录：$output_dir"

if [[ -z $input_file && -t 0 ]]; then
    printf '请输入下载链接，每行一个；输入完成后按 Ctrl-D 开始下载：\n' >&2
fi

total=0
downloaded=0
failed=0
invalid=0
skipped=0
line_no=0

download_one() {
    local url=$1
    local url_path filename target part

    # 去掉查询参数和片段，只用 URL 路径最后一段作为文件名。
    url_path=${url%%\?*}
    url_path=${url_path%%\#*}
    filename=${url_path##*/}

    if [[ -z $filename || $filename == '.' || $filename == '..' ]]; then
        filename="download_${line_no}"
    fi

    target="${output_dir%/}/$filename"
    part="${target}.part"
    total=$((total + 1))

    if [[ -e $target ]]; then
        skipped=$((skipped + 1))
        printf '[%d] 跳过已存在文件：%s\n' "$total" "$target"
        return 0
    fi

    printf '[%d] 下载：%s\n    保存到：%s\n' "$total" "$url" "$target"

    # 失败时保留 .part，成功后再改成最终文件名，避免残片被误当成完整文件。
    if wget --continue \
        --tries=3 \
        --timeout=30 \
        --waitretry=5 \
        --output-document="$part" \
        "$url" && mv -f "$part" "$target"; then
        downloaded=$((downloaded + 1))
    else
        failed=$((failed + 1))
        printf '    下载失败，保留 %s 以便下次续传。\n' "$part" >&2
    fi
}

process_input() {
    local raw line

    while IFS= read -r raw || [[ -n $raw ]]; do
        line_no=$((line_no + 1))
        raw=${raw%$'\r'}
        line=$(trim "$raw")

        # 忽略空行和以 # 开头的注释行。
        if [[ -z $line || ${line:0:1} == '#' ]]; then
            continue
        fi

        if [[ ! $line =~ ^(https?|ftp):// ]]; then
            invalid=$((invalid + 1))
            printf '跳过第 %d 行：不是支持的 URL（仅支持 http/https/ftp）。\n' "$line_no" >&2
            continue
        fi

        download_one "$line"
    done
}

if [[ -n $input_file && $input_file != '-' ]]; then
    process_input < "$input_file"
else
    process_input
fi

printf '\n完成：成功 %d，跳过 %d，失败 %d，无效链接 %d，输出目录：%s\n' \
    "$downloaded" "$skipped" "$failed" "$invalid" "$output_dir"

if (( failed > 0 )); then
    exit 1
fi
