#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
mkdir -p build/app
cat > build/app/aligned.h <<'EOF'
void* _aligned_malloc(unsigned long s,unsigned long a);
void _aligned_free(void*p);
EOF
cat > build/app/aligned.c <<'EOF'
#include <stdlib.h>
#include <malloc.h>
void* _aligned_malloc(unsigned long s,unsigned long a){return memalign(a,s);}
void _aligned_free(void*p){free(p);}
EOF
clang -std=c11 -O1 -w -fshort-wchar -DGU_UEFI_PORT -D__KERNEL__ \
  -include stdlib.h -include build/app/aligned.h \
  -Icompat -I../deps/edk2/MdePkg/Include -I../deps/edk2/MdePkg/Include/X64 \
  tests/test_app.c app.c app_io.c ownership.c format.c sha256.c checks.c \
  port.c compat.c vendor/unlock_610.c vendor/prep_610.c vendor/layout_610.c \
  vendor/vbios_610.c vendor/gfw_eval.c build/app/aligned.c -o build/app/app-tests
./build/app/app-tests ../evidence/boot-03/PROM.BIN ../analysis/pinned-assets \
  ../evidence/boot-03/REPORT.TXT "$@"
