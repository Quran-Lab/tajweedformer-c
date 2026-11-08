/* cpu_detect: runs the int8 tier detection only (no kernel) and prints the decision with the raw CPUID and XCR0
 * bits it read (the bits are listed in src/zf_gemm_vnni.c). Exit 0 unless the decision contradicts the bits. */
#include <stdio.h>
#include <string.h>

#include "zf_gemm_vnni.h"

int main(void) {
  char buf[1024];
  const int t = zf_int8_cpu_report(buf, sizeof buf);
  printf("%s\n", buf);
  if (t != zf_int8_cpu_tier()) {
    printf("FAIL: the report (%s) and zf_int8_cpu_tier (%s) disagree\n", zf_int8_tier_name(t), zf_int8_tier_name(zf_int8_cpu_tier()));
    return 1;
  }
  printf("PASS: int8 tier %s\n", zf_int8_tier_name(t));
  return 0;
}
