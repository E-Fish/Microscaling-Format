`timescale 1ns / 1ps

// Registers inputs and outputs around the combinational mxfp6_mult core
// so Vivado reports a clean reg-to-reg path through the multiplier.
module mxfp6_wrapper #(
    parameter int N = 32
)(
    input  logic             clk,
    input  logic [8+6*N-1:0] a,
    input  logic [8+6*N-1:0] b,
    output logic [8+6*N-1:0] c
);
    localparam int W = 8 + 6*N;

    logic [W-1:0] a_tmp;
    logic [W-1:0] b_tmp;
    logic [W-1:0] out_tmp;

    mxfp6_mult #(
        .N(N)
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
