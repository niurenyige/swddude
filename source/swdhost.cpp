/*
 * Copyright (c) 2012, Anton Staaf, Cliff L. Biffle.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the project nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "target.h"
#include "swd_dp.h"
#include "swd_mpsse.h"
#include "swd.h"

#include "rptr.h"

#include "armv6m_v7m.h"
#include "arm.h"

#include "libs/error/error_stack.h"
#include "libs/log/log_default.h"
#include "libs/command_line/command_line.h"

#include <vector>

#include <unistd.h>
#include <stdio.h>
#include <ftdi.h>

using namespace Log;
using Err::Error;

using namespace ARM;
using namespace ARMv6M_v7M;

using std::vector;


/*******************************************************************************
 * Command line flags
 */
namespace CommandLine
{
    static Scalar<int>
    debug("debug", true, 0, "What level of debug logging to use.");

    static Scalar<String>
    programmer("programmer", true, "um232h", "FTDI-based programmer to use");

    static Scalar<int> vid("vid", true, 0, "FTDI VID");
    static Scalar<int> pid("pid", true, 0, "FTDI PID");

    static Scalar<int>
    interface("interface", true, 0, "Interface on FTDI chip");

    static Argument * arguments[] =
    {
        &debug,
        &programmer,
        &vid,
        &pid,
        &interface,
        NULL
    };
}

/*******************************************************************************
 * Implements the semihosting SYS_WRITEC operation.
 */
Error write_char(Target & target, word_t parameter)
{
    putchar(parameter);
    fflush(stdout);
    return Err::success;
}

/*******************************************************************************
 * Inspects the CPU's halt conditions to see whether semihosting has been
 * invoked.
 */
Error handle_halt(Target & target)
{
    word_t dfsr;
    CheckRetry(target.read_word(SCB::DFSR, &dfsr), 100);

    if ((dfsr & SCB::DFSR_reason_mask) == SCB::DFSR_BKPT)
    {
        word_t pc;
        CheckRetry(target.read_register(Register::PC, &pc), 100);

        /*
         * Targets may only support 32-bit accesses, but the PC is 16-bit
         * aligned.  Find the address of the word containing the current
         * instruction and load the whole thing.
         */
        rptr<word_t> instr_word_address(pc & ~0x3);
        word_t instr_word;
        CheckRetry(target.read_word(instr_word_address, &instr_word), 100);

        /*
         * Extract the instruction halfword from the word we've read.
         */
        halfword_t instr = pc & 2 ? instr_word >> 16 : instr_word & 0xFFFF;

        if (instr == 0xBEAB)
        {
            /*
             * The semihosting ABI, summarized, goes something like this:
             *  - Operation code in R0.
             *  - Single 32-bit parameter, or pointer to memory block containing
             *    more parameters, in R1.
             *  - Return value in R0 (either 32-bit value or pointer).
             */
            word_t operation;
            CheckRetry(target.read_register(Register::R0, &operation), 100);

            word_t parameter;
            CheckRetry(target.read_register(Register::R1, &parameter), 100);

            switch (operation)
            {
                case 0x3:
                    Check(write_char(target, parameter));
                    break;

                default:
                    warning("Unsupported semihosting operation 0x%X",
                            operation);
                    return Err::failure;
            }

            /*
             * Success!  Advance target PC past the breakpoint and resume.
             */
            pc += 2;
            CheckRetry(target.write_register(Register::PC, pc), 100);
            Check(target.resume());

            return Err::success;
        }
        else
        {
            warning("Unexpected non-semihosting breakpoint %04X @%08X",
                    instr,
                    pc);
            return Err::failure;
        }
    }
    else
    {
        warning("Processor halted for unexpected reason 0x%X", dfsr);
        return Err::failure;
    }
}

/*******************************************************************************
 * Semihosting tool entry point.
 */
Error host_main(SWDDriver & swd)
{
    DebugAccessPort dap(swd);
    Target target(swd, dap, 0);

    uint32_t idcode;
    Check(swd.initialize(&idcode));

    Check(swd.enter_reset());
    usleep(10000);
    Check(dap.reset_state());
    Check(target.initialize());
    Check(target.reset_halt_state());

    Check(swd.leave_reset());

    while (true)
    {
        word_t dhcsr;
        CheckRetry(target.read_word(DCB::DHCSR, &dhcsr), 100);

        if (dhcsr & DCB::DHCSR_S_HALT)
        {
            Check(handle_halt(target));
        }
    }
     

    return Err::success;
}

static struct {
    char const * name;
    MPSSEConfig const * config;
} const programmer_table[] = {
    { "um232h",      &um232h_config      },
    { "bus_blaster", &bus_blaster_config },
};

static size_t const programmer_table_count =
    sizeof(programmer_table) / sizeof(programmer_table[0]);

static Error lookup_programmer(String name, MPSSEConfig const * * config) {
    for (size_t i = 0; i < programmer_table_count; ++i) {
        if (name.equal(programmer_table[i].name))
        {
            *config = programmer_table[i].config;
            return Err::success;
        }
    }

    return Err::failure;
}

/*******************************************************************************
 * Entry point (sort of -- see main below)
 */

static Error error_main(int argc, char const * * argv)
{
    Error                  check_error = Err::success;
    libusb_context *       libusb;
    libusb_device_handle * handle;
    libusb_device *        device;
    ftdi_context           ftdi;
    MPSSEConfig const *    config;

    Check(lookup_programmer(CommandLine::programmer.get(), &config));

    ftdi_interface interface = ftdi_interface(INTERFACE_A +
                                              config->default_interface);
    uint16_t       vid       = config->default_vid;
    uint16_t       pid       = config->default_pid;

    if (CommandLine::interface.set())
        interface = ftdi_interface(INTERFACE_A + CommandLine::interface.get());

    if (CommandLine::vid.set())
        vid = CommandLine::vid.get();

    if (CommandLine::pid.set())
        pid = CommandLine::pid.get();

    CheckCleanupP(libusb_init(&libusb), libusb_init_failed);
    CheckCleanupP(ftdi_init(&ftdi), ftdi_init_failed);

    /*
     * Locate FTDI chip using it's VID:PID pair.  This doesn't uniquely identify
     * the programmer so this will need to be improved.
     */
    handle = libusb_open_device_with_vid_pid(libusb, vid, pid);

    CheckCleanupStringB(handle, libusb_open_failed,
                        "No device found with VID:PID = 0x%04x:0x%04x\n",
                        vid, pid);

    CheckCleanupB(device = libusb_get_device(handle), get_failed);

    /*
     * The interface must be selected before the ftdi device can be opened.
     */
    CheckCleanupStringP(ftdi_set_interface(&ftdi, interface),
                        interface_failed,
                        "Unable to set FTDI device interface: %s",
                        ftdi_get_error_string(&ftdi));

    CheckCleanupStringP(ftdi_usb_open_dev(&ftdi, device),
                        open_failed,
                        "Unable to open FTDI device: %s",
                        ftdi_get_error_string(&ftdi));

    CheckCleanupStringP(ftdi_usb_reset(&ftdi),
                        reset_failed,
                        "FTDI device reset failed: %s",
                        ftdi_get_error_string(&ftdi));

    {
        unsigned chipid;

        CheckCleanupP(ftdi_read_chipid(&ftdi, &chipid), read_failed);

        debug(3, "FTDI chipid: %X", chipid);
    }

    {
        MPSSESWDDriver swd(*config, &ftdi);
        CheckCleanup(host_main(swd), host_failed);
    }

host_failed:
    CheckP(ftdi_set_bitmode(&ftdi, 0xFF, BITMODE_RESET));

read_failed:
reset_failed:
    CheckStringP(ftdi_usb_close(&ftdi),
                 "Unable to close FTDI device: %s",
                 ftdi_get_error_string(&ftdi));

open_failed:
interface_failed:
get_failed:
    libusb_close(handle);

libusb_open_failed:
    ftdi_deinit(&ftdi);

ftdi_init_failed:
    libusb_exit(libusb);

libusb_init_failed:
    return check_error;
}

/******************************************************************************/
int main(int argc, char const * * argv)
{
    Error check_error = Err::success;

    CheckCleanup(CommandLine::parse(argc, argv, CommandLine::arguments),
                 failure);

    log().set_level(CommandLine::debug.get());

    CheckCleanup(error_main(argc, argv), failure);
    return 0;

failure:
    Err::stack()->print();
    return 1;
}
/******************************************************************************/