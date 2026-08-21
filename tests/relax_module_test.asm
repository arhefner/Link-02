;
; relax_module_test.asm - regression test for link02's -r (branch
; relaxation) combined with -m (loadable-module fixup table) output.
;
; Motivation: -m's fixup table records the FINAL, post-relaxation
; address of every remaining absolute reference (an unshrunk long
; branch, or an external symbol reference) so a runtime loader can
; add its own load base to each one. Relaxation, by design, rewrites
; a proc's own layout across several rounds as branches shrink from
; 3 bytes to 2 -- every address after a shrunk branch moves. The
; question this test exists to answer: does the fixup table end up
; describing the FINAL layout, or could it describe a STALE,
; intermediate round's layout instead?
;
; Design: a long chain of trivially-adjacent "lbr next_label"
; branches (tm_l0 through tm_l79), each of which SHOULD be shrinkable
; to a short branch -- followed by one final branch, tm_far_branch,
; to a target (tm_far_target) placed far enough away (2000 bytes)
; that it can NEVER shrink, guaranteeing at least one real fixup
; survives to the final module regardless of round count. The chain
; length (80) is large enough that, empirically, at least one branch
; genuinely fails to shrink on its first attempt and needs a real
; exclude-and-retry round (confirmed via this exact file: "80 of 81
; local long branches shortened (2 rounds)") -- i.e. this is NOT a
; simulated multi-round scenario (e.g. via RLX_MAX_SHRINK), it is a
; real one, driven by actual short-branch-range boundaries.
;
; tm_l0 and tm_far_target are made public so the verification script
; can look up their real, final, linked addresses directly (via
; link02 -s) rather than assuming a fixed address.
;
; Usage: assemble with `asm02 -L -C -r relax_module_test.asm`, then
; link with `link02 -b -be -r -m -o relax_module_test.bin -s
; relax_module_test.prg`. See run_relax_module_test.py for the
; automated version of this, which decodes the resulting binary and
; fixup table and confirms they are self-consistent.
;

#include    opcodes.def

            org     0

            proc    tm_main

tm_l0:      lbr     tm_l1
tm_l1:      lbr     tm_l2
tm_l2:      lbr     tm_l3
tm_l3:      lbr     tm_l4
tm_l4:      lbr     tm_l5
tm_l5:      lbr     tm_l6
tm_l6:      lbr     tm_l7
tm_l7:      lbr     tm_l8
tm_l8:      lbr     tm_l9
tm_l9:      lbr     tm_l10
tm_l10:     lbr     tm_l11
tm_l11:     lbr     tm_l12
tm_l12:     lbr     tm_l13
tm_l13:     lbr     tm_l14
tm_l14:     lbr     tm_l15
tm_l15:     lbr     tm_l16
tm_l16:     lbr     tm_l17
tm_l17:     lbr     tm_l18
tm_l18:     lbr     tm_l19
tm_l19:     lbr     tm_l20
tm_l20:     lbr     tm_l21
tm_l21:     lbr     tm_l22
tm_l22:     lbr     tm_l23
tm_l23:     lbr     tm_l24
tm_l24:     lbr     tm_l25
tm_l25:     lbr     tm_l26
tm_l26:     lbr     tm_l27
tm_l27:     lbr     tm_l28
tm_l28:     lbr     tm_l29
tm_l29:     lbr     tm_l30
tm_l30:     lbr     tm_l31
tm_l31:     lbr     tm_l32
tm_l32:     lbr     tm_l33
tm_l33:     lbr     tm_l34
tm_l34:     lbr     tm_l35
tm_l35:     lbr     tm_l36
tm_l36:     lbr     tm_l37
tm_l37:     lbr     tm_l38
tm_l38:     lbr     tm_l39
tm_l39:     lbr     tm_l40
tm_l40:     lbr     tm_l41
tm_l41:     lbr     tm_l42
tm_l42:     lbr     tm_l43
tm_l43:     lbr     tm_l44
tm_l44:     lbr     tm_l45
tm_l45:     lbr     tm_l46
tm_l46:     lbr     tm_l47
tm_l47:     lbr     tm_l48
tm_l48:     lbr     tm_l49
tm_l49:     lbr     tm_l50
tm_l50:     lbr     tm_l51
tm_l51:     lbr     tm_l52
tm_l52:     lbr     tm_l53
tm_l53:     lbr     tm_l54
tm_l54:     lbr     tm_l55
tm_l55:     lbr     tm_l56
tm_l56:     lbr     tm_l57
tm_l57:     lbr     tm_l58
tm_l58:     lbr     tm_l59
tm_l59:     lbr     tm_l60
tm_l60:     lbr     tm_l61
tm_l61:     lbr     tm_l62
tm_l62:     lbr     tm_l63
tm_l63:     lbr     tm_l64
tm_l64:     lbr     tm_l65
tm_l65:     lbr     tm_l66
tm_l66:     lbr     tm_l67
tm_l67:     lbr     tm_l68
tm_l68:     lbr     tm_l69
tm_l69:     lbr     tm_l70
tm_l70:     lbr     tm_l71
tm_l71:     lbr     tm_l72
tm_l72:     lbr     tm_l73
tm_l73:     lbr     tm_l74
tm_l74:     lbr     tm_l75
tm_l75:     lbr     tm_l76
tm_l76:     lbr     tm_l77
tm_l77:     lbr     tm_l78
tm_l78:     lbr     tm_l79
tm_l79:     lbr     tm_l80
tm_l80:
tm_far_branch:
            lbr     tm_far_target
            rtn

            ds      2000

tm_far_target:
            rtn

            public  tm_l0
            public  tm_far_target

            endp

            end     tm_l0
