#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    // 检查参数数量
    if (argc != 2) {
        fprintf(2, "Usage: sleep ticks\n");
        exit(1);
    }

    // 将字符串参数转换为整数
    int ticks = atoi(argv[1]);
    
    // 调用 sleep 系统调用
    sleep(ticks);
    
    // 程序执行完毕，正常退出
    exit(0);
}