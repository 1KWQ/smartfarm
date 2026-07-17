#include "utils.h"

/**
 * 将浮点数转换为整数和小数部分（保留1位小数）
 * @param value 浮点数值
 * @param intPart 输出的整数部分指针
 * @param decPart 输出的小数部分指针（0-9）
 */
void floatToIntDec(float value, int *intPart, int *decPart) {
  *intPart = (int)value;
  float decimal = value - (float)(*intPart);
  if (decimal < 0) {
    decimal = -decimal; // 处理负数的小数部分
  }
  *decPart = (int)(decimal * 10 + 0.5f); // 四舍五入到1位小数
  if (*decPart >= 10) {
    *decPart = 0;
    (*intPart)++;
  } else if (*decPart < 0) {
    *decPart = 0;
  }
}