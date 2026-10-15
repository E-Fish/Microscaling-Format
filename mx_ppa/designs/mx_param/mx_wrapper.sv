`timescale 1ns / 1ps

// Registers inputs and outputs around the combinational mx_mult core
// so Vivado reports a clean reg-to-reg path through the multiplier.
//
//   MXFP4 E2M1: EXP=2 MAN=1    MXFP6 E2M3: EXP=2 MAN=3    MXFP6 E3M2: EXP=3 MAN=2
//   MXFP8 E4M3: EXP=4 MAN=3    MXFP8 E5M2: EXP=5 MAN=2
module mx_wrapper #(
    parameter int EXP = 2,
    parameter int MAN = 3,
    parameter int N   = 32
)(
    input  logic                        clk,
    input  logic [8+N*(1+EXP+MAN)-1:0]  a,
    input  logic [8+N*(1+EXP+MAN)-1:0]  b,
    output logic [8+N*(1+EXP+MAN)-1:0]  c
);
    localparam int W = 8 + N*(1+EXP+MAN);

    logic [W-1:0] a_tmp;
    logic [W-1:0] b_tmp;
    logic [W-1:0] out_tmp;

    mx_mult #(
        .EXP(EXP),
        .MAN(MAN),
        .N  (N)
    ) u0 (
        .a  (a_tmp),
        .b  (b_tmp),
        .out(out_tmp)
    );

    always_ff @(posedge clk) begin
        a_tmp <= a;
        b_tmp <= b;
        c     <= out_tmp;
    end

endmodule
