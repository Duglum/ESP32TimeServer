// ESP32 Time Server (from ESP32-WebTherm)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// Stop of the EMAC before every CPU reset and at the start of the firmware, see emac_halt.h.
//
// ESP-IDF 6.1.1 restarts with a CPU reset (esp_restart_noos(): rst:0xc SW_CPU_RESET). Its
// esp_system_reset_modules_on_exit() (port/soc/esp32p4/system_internal.c) stops or resets the other DMA units (AXI,
// 2D, DW and AHB DMA, the DMA of the SDMMC host "to avoid memory corruption after restart"), but not the EMAC, and
// esp_eth registers no shutdown handler. The EMAC's DMA then goes on storing received frames into the receive
// buffers of the old firmware until the new one creates its driver (esp_eth_mac_new_esp32() resets the EMAC, about
// 0.8 s after the reset on the test board, 0.35 s after the new firmware cleared its bss). When the new image has more
// bss, those buffers are its variables or the first blocks of its heap, and any host on the LAN can write its bytes
// into them during the restart.
//
// Two points of ESP-IDF are wrapped with the linker (CMakeLists.txt, -Wl,--wrap), because it has no documented
// hook there: a shutdown handler (esp_register_shutdown_handler) runs in esp_restart() only, not in the restart
// after a panic, and nothing of the application runs at the start before the system init functions. If ESP-IDF
// renames a wrapped function, __real_<name> is undefined and the link fails.
//  - esp_restart_noos(): every CPU reset of ESP-IDF goes through it (esp_restart() after the shutdown handlers,
//    panic_restart() after the core dump; no other caller in this firmware's image). Interrupts off, the other core
//    stalled (both as the function itself does next), then the EMAC is stopped and reset.
//  - cache_hal_init(): the first call of call_start_cpu0() after init_bss(), nothing of the application runs earlier.
//    The EMAC is stopped and reset there when the firmware before left it running (an older firmware, a reset that
//    bypasses esp_restart_noos()). A frame stored while init_bss() runs (about 0.7 ms on the test board) can still
//    end up in the bss; closing that needs the bootloader. If ESP-IDF stops calling cache_hal_init(),
//    start_ethernet_driver() sees emac_start_state_t::not_checked, logs it and stops the EMAC before it creates the
//    driver.
// The system reset of the ROM (esp_rom_software_reset_system(), the brownout interrupt, rst:0x3) and the RTC
// watchdog's system reset (rst:0x9) bypass esp_restart_noos(); measured on the test board, both leave the EMAC in its
// reset state.
// Both run before the cache and the scheduler can be relied on: IRAM code and register access only, no lock, no
// log, no esp_eth call, a bounded wait. Running twice does no harm.
//
// The stop is the sequence of emac_hal_stop() without its refusals: transmit DMA, MAC transmitter and receiver,
// receive DMA, interrupt enables; each DMA ends the frame it is transferring and then reports "stopped" (dmastatus).
// Then the EMAC's reset is pulsed (as emac_ll_reset_register()) so that it is in its reset state with its DMA
// stopped. The reset waits for the DMA, as the AXI bus does not allow a transaction to be cut (system_internal.c);
// when the DMA does not stop within STOP_WAIT_US (the receive hang that main.cpp restarts for), the reset is pulsed
// anyway, as the driver itself does at every start. The bus clock is left as it is: the driver enables it and pulses
// the reset itself when it is created, so the PTP clock, the time stamping and the DMA operation mode (DFF) are set
// up again from the reset state as after every restart before.
// Registers per target as in ESP-IDF's hal/emac_ll.h (emac_ll_enable_bus_clock(), emac_ll_reset_register()); the
// ESP32-S31 variant is compiled only, it never ran on hardware.

#include "emac_halt.h"

extern "C"
{
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "hal/cache_hal.h"
#include "riscv/rv_utils.h"
#include "sdkconfig.h"
#include "soc/emac_dma_struct.h"
#include "soc/emac_mac_struct.h"
#include "soc/hp_sys_clkrst_struct.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "soc/lp_clkrst_struct.h"
#elif CONFIG_IDF_TARGET_ESP32S31
#include "soc/cnnt_sys_struct.h"
#else
#error "emac_halt.cpp: the EMAC clock and reset registers of this target are not known"
#endif
}

namespace
{
    // Longer than the transfer of a frame of 1522 bytes at 10 Mbit/s (1.2 ms)
    constexpr uint32_t STOP_WAIT_US = 10000;
    constexpr uint32_t STOP_POLL_US = 10;

    // In bss: cleared by init_bss() before the check at the start
    emac_start_state_t s_start_state = emac_start_state_t::not_checked;

#if CONFIG_IDF_TARGET_ESP32P4
    IRAM_ATTR bool clock_on() { return HP_SYS_CLKRST.soc_clk_ctrl1.reg_emac_sys_clk_en != 0; }
    IRAM_ATTR bool in_reset() { return LP_AON_CLKRST.hp_sdmmc_emac_rst_ctrl.rst_en_emac != 0; }
    IRAM_ATTR void reset_pulse()
    {
        LP_AON_CLKRST.hp_sdmmc_emac_rst_ctrl.rst_en_emac = 1;
        LP_AON_CLKRST.hp_sdmmc_emac_rst_ctrl.rst_en_emac = 0;
    }
#else
    IRAM_ATTR bool clock_on() { return HP_SYS_CLKRST.emac_ctrl0.reg_emac_sys_clk_en != 0; }
    IRAM_ATTR bool in_reset() { return CNNT_SYS_REG.sys_hp_emac_ctrl.sys_emac_rst_en != 0; }
    IRAM_ATTR void reset_pulse()
    {
        CNNT_SYS_REG.sys_hp_emac_ctrl.sys_emac_rst_en = 1;
        CNNT_SYS_REG.sys_hp_emac_ctrl.sys_emac_rst_en = 0;
    }
#endif

    IRAM_ATTR bool dma_stopped()
    {
        return EMAC_DMA.dmastatus.recv_proc_state == 0 && EMAC_DMA.dmastatus.trans_proc_state == 0;
    }

    // Returns true when the DMA was started. The EMAC's DMA runs only with its bus clock on and its reset released;
    // its registers are touched only then (whether an access with the clock off or the reset asserted can hang is not
    // known).
    IRAM_ATTR bool halt()
    {
        if (!clock_on() || in_reset())
            return false;
        const bool started = EMAC_DMA.dmaoperation_mode.start_stop_rx != 0 ||
                             EMAC_DMA.dmaoperation_mode.start_stop_transmission_command != 0 || !dma_stopped();
        EMAC_DMA.dmaoperation_mode.start_stop_transmission_command = 0;
        EMAC_MAC.gmacconfig.rx = 0;
        EMAC_MAC.gmacconfig.tx = 0;
        EMAC_DMA.dmaoperation_mode.start_stop_rx = 0;
        EMAC_DMA.dmain_en.val = 0;
        for (uint32_t waited_us = 0; waited_us < STOP_WAIT_US && !dma_stopped(); waited_us += STOP_POLL_US)
            esp_rom_delay_us(STOP_POLL_US);
        reset_pulse();
        return started;
    }
} // namespace

extern "C"
{
    [[noreturn]] void __real_esp_restart_noos(void);
    void __real_cache_hal_init(const cache_hal_config_t *config);

    [[noreturn]] IRAM_ATTR void __wrap_esp_restart_noos(void)
    {
        rv_utils_intr_global_disable();
#if !CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
        // The other core cannot start the EMAC again (esp_eth_start()) while it is stopped
        esp_cpu_stall(esp_cpu_get_core_id() == 0 ? 1 : 0);
#endif
        halt();
        __real_esp_restart_noos();
    }

    IRAM_ATTR void __wrap_cache_hal_init(const cache_hal_config_t *config)
    {
        if (s_start_state == emac_start_state_t::not_checked)
            s_start_state = halt() ? emac_start_state_t::stopped : emac_start_state_t::idle;
        __real_cache_hal_init(config);
    }
}

emac_start_state_t emac_halt_start_state()
{
    return s_start_state;
}

bool emac_halt_now()
{
    return halt();
}
