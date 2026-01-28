#!/bin/bash
# A script to run the extractor.py utility on specified files
#!/bin/bash
DOT_TIMEOUT=5

SVF_PTA_FLAGS=("-nander" "-sander" "-sfrander" "-ander" "-steens" "-fspta" "-vfspta" "-type")
SVF_PTA_FLAG="-ander"

SVF_PATH=""
LLVM_PATH=""

function run_base(){
    filepath="$1"
    # 1. Internalize all functions except main (marks them as internal)
    # 2. Run Global Dead Code Elimination (removes internal, uncalled functions)
    "$LLVM_PATH/opt" -S -passes='internalize,globaldce,early-cse' -internalize-public-api-list=main $filepath.bc -o $filepath.opt.bc
    # 3. Run dvf/wpa on the optimized bitcode
    echo "Processing $file with DVF"
    "$SVF_PATH/dvf" -query=all -cpts -cxt -print-all-pts -print-pag -dump-callgraph -max-cxt=3 -flow-bg=10000 -cxt-bg=10000 $filepath.opt.bc > $filepath.pta
    # echo "Processing $file with SVF flags: $SVF_PTA_FLAG"
    # "$SVF_PATH/wpa" $SVF_PTA_FLAG -print-all-pts -print-pag -dump-callgraph $filepath.opt.bc > $filepath.pta;
    python3 extractor.py $filepath;
    # rm $filepath.pta;
    rm $filepath.bc
    rm $filepath.opt.bc
    rm callgraph_initial.dot
    rm callgraph_final.dot
}

function run_c(){
    directoryname=$(dirname "$1")
    filename=$(basename "$1" .c)
    filepath="$directoryname/$filename"

    if [[ -z "$LLVM_PATH" ]]; then
        echo "Error: LLVM path is required to process .c files." >&2
        exit 1
    fi

    "$LLVM_PATH/clang" -g -Xclang -disable-O0-optnone -fno-discard-value-names -emit-llvm -c $filepath.c -o $filepath.bc;
    # ../llvm-project/build/bin/opt -passes=instnamer -S $filename.bc -o $filename.bc
    # ../llvm-project/build/bin/llvm-dis $filepath.bc -o $filepath.ll;
    run_base "$filepath";    
}

function run_cpp(){
    directoryname=$(dirname "$1")
    filename=$(basename "$1")
    filename="${filename%.cpp}"
    filename="${filename%.cc}"
    filepath="$directoryname/$filename"

    if [[ -z "$LLVM_PATH" ]]; then
        echo "Error: LLVM path is required to process C++ files." >&2
        exit 1
    fi

    "$LLVM_PATH/clang++" -g -Xclang -disable-O0-optnone -fno-discard-value-names -emit-llvm -c "$1" -o $filepath.bc;
    # ../llvm-project/build/bin/opt -passes=instnamer -S $filename.bc -o $filename.bc
    # ../llvm-project/build/bin/llvm-dis $filepath.bc -o $filepath.ll;
    run_base "$filepath";    
}


function run_bc(){
    directoryname=$(dirname "$1")
    filename=$(basename "$1" .bc)
    filepath="$directoryname/$filename"
    run_base "$filepath";
}

function run_ll(){
    directoryname=$(dirname "$1")
    filename=$(basename "$1" .ll)
    filepath="$directoryname/$filename"

    if [[ -z "$LLVM_PATH" ]]; then
        echo "Error: LLVM path is required to process .ll files." >&2
        exit 1
    fi

    "$LLVM_PATH/llvm-as" $filepath.ll -o $filepath.bc;
    run_base "$filepath";
}

function run(){
    local filename="$1"
    if [[ "$filename" == *.c ]]; then
        run_c "$filename"
    elif [[ "$filename" == *.cpp || "$filename" == *.cc ]]; then
        run_cpp "$filename"
    elif [[ "$filename" == *.bc ]]; then
        run_bc "$filename"
    elif [[ "$filename" == *.ll ]]; then
        run_ll "$filename"
    else
        echo "Unknown file type: $filename"
    fi
}

function usage() {
    echo "Usage: $0 --svf-path <path> [--llvm-path <path>] [SVF_FLAG] <file1> [file2 ...]"
    echo
    echo "Options:"
    echo "  --svf-path     Path to SVF build/bin directory (required)"
    echo "  --llvm-path    Path to LLVM build/bin directory (required for .c and .ll)"
    echo "  SVF_FLAG       One of the following (optional, default: -ander):"
    for flag in "${SVF_PTA_FLAGS[@]}"; do
        echo "    $flag"
    done
    exit 1
}

function validate_flag() {
    local flag="$1"
    for valid_flag in "${SVF_PTA_FLAGS[@]}"; do
        if [[ "$flag" == "$valid_flag" ]]; then
            return 0  # Valid
        fi
    done
    return 1  # Invalid
}

if [[ $# -eq 0 ]]; then
    usage
fi

# Parse arguments
args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --svf-path)
            SVF_PATH="$2"
            shift 2
            ;;
        --llvm-path)
            LLVM_PATH="$2"
            shift 2
            ;;
        -*)
            if validate_flag "$1"; then
                SVF_PTA_FLAG="$1"
                shift
            else
                echo "Error: Invalid flag '$1'"
                usage
            fi
            ;;
        *)
            args+=("$1")
            shift
            ;;
    esac
done

# Restore positional arguments
set -- "${args[@]}"

# Validations
if [[ -z "$SVF_PATH" ]]; then
    echo "Error: --svf-path is required."
    usage
fi

if [[ $# -eq 0 ]]; then
    echo "Error: No files provided"
    usage
fi


for file in "$@"; do
    if [[ ! -f "$file" ]]; then
        echo "Error: File '$file' does not exist or is not a regular file" >&2
        continue  # Skip to next file
    fi

    run "$file"
done
