/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * @file v3d_opt_dead_code.c
 *
 * This is a simple dead code eliminator for SSA values in VIR.
 *
 * It walks all the instructions finding what temps are used, then walks again
 * to remove instructions writing unused temps.
 *
 * This is an inefficient implementation if you have long chains of
 * instructions where the entire chain is dead, but we expect those to have
 * been eliminated at the NIR level, and here we're just cleaning up small
 * problems produced by NIR->VIR.
 */

#include "v3d_compiler.h"

static bool debug;

static void
dce(struct v3d_compile *c, struct qinst *inst)
{
        if (debug) {
                fprintf(stderr, "Removing: ");
                vir_dump_inst(c, inst);
                fprintf(stderr, "\n");
        }
        assert(!v3d_qpu_writes_flags(&inst->qpu));
        vir_remove_instruction(c, inst);
}

static bool
has_nonremovable_reads(struct v3d_compile *c, struct qinst *inst)
{
        for (int i = 0; i < vir_get_nsrc(inst); i++) {
                if (inst->src[i].file == QFILE_VPM)
                        return true;
        }

        return false;
}

static bool
can_write_to_null(struct v3d_compile *c, struct qinst *inst)
{
        /* The SFU instructions must write to a physical register. */
        if (c->devinfo->ver >= 41 && v3d_qpu_uses_sfu(&inst->qpu))
                return false;

        return true;
}

static void
vir_dce_flags(struct v3d_compile *c, struct qinst *inst)
{
        if (debug) {
                fprintf(stderr,
                        "Removing flags write from: ");
                vir_dump_inst(c, inst);
                fprintf(stderr, "\n");
        }

        assert(inst->qpu.type == V3D_QPU_INSTR_TYPE_ALU);

        inst->qpu.flags.apf = V3D_QPU_PF_NONE;
        inst->qpu.flags.mpf = V3D_QPU_PF_NONE;
        inst->qpu.flags.auf = V3D_QPU_UF_NONE;
        inst->qpu.flags.muf = V3D_QPU_UF_NONE;
}

bool
vir_opt_dead_code(struct v3d_compile *c)
{
        bool progress = false;
        bool *used = calloc(c->num_temps, sizeof(bool));

        /* Defuse the "are you removing the cursor?" assertion in the core.
         * You'll need to set up a new cursor for any new instructions after
         * doing DCE (which we would expect, anyway).
         */
        c->cursor.link = NULL;

        vir_for_each_inst_inorder(inst, c) {
                for (int i = 0; i < vir_get_nsrc(inst); i++) {
                        if (inst->src[i].file == QFILE_TEMP)
                                used[inst->src[i].index] = true;
                }
        }

        vir_for_each_block(block, c) {
                struct qinst *last_flags_write = NULL;

                vir_for_each_inst_safe(inst, block) {
                        /* If this instruction reads the flags, we can't
                         * remove the flags generation for it.
                         */
                        if (v3d_qpu_reads_flags(&inst->qpu))
                                last_flags_write = NULL;

                        if (inst->dst.file != QFILE_NULL &&
                            !(inst->dst.file == QFILE_TEMP &&
                              !used[inst->dst.index])) {
                                continue;
                        }

                        if (vir_has_side_effects(c, inst))
                                continue;

                        if (v3d_qpu_writes_flags(&inst->qpu)) {
                                /* If we obscure a previous flags write,
                                 * drop it.
                                 */
                                if (last_flags_write &&
                                    (inst->qpu.flags.apf != V3D_QPU_PF_NONE ||
                                     inst->qpu.flags.mpf != V3D_QPU_PF_NONE)) {
                                        vir_dce_flags(c, last_flags_write);
                                        progress = true;
                                }

                                last_flags_write = inst;
                        }

                        if (v3d_qpu_writes_flags(&inst->qpu) ||
                            has_nonremovable_reads(c, inst)) {
                                /* If we can't remove the instruction, but we
                                 * don't need its destination value, just
                                 * remove the destination.  The register
                                 * allocator would trivially color it and it
                                 * wouldn't cause any register pressure, but
                                 * it's nicer to read the VIR code without
                                 * unused destination regs.
                                 */
                                if (inst->dst.file == QFILE_TEMP &&
                                    can_write_to_null(c, inst)) {
                                        if (debug) {
                                                fprintf(stderr,
                                                        "Removing dst from: ");
                                                vir_dump_inst(c, inst);
                                                fprintf(stderr, "\n");
                                        }
                                        c->defs[inst->dst.index] = NULL;
                                        inst->dst.file = QFILE_NULL;
                                        progress = true;
                                }
                                continue;
                        }

                        assert(inst != last_flags_write);
                        dce(c, inst);
                        progress = true;
                        continue;
                }
        }

        free(used);

        return progress;
}
