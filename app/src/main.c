#include <rtthread.h>

#include "demo_engine.h"

int main(void)
{
    rt_kprintf("\n[app] SF32LB57 + P045VR04-IPS | GE demo suite\n");

    demo_core_init();
    demo_core_start();

    return 0;
}
