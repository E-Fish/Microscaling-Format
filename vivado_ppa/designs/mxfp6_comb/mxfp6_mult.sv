// mx floating point 6 multiplier, for E2M3 FP6

`timescale 1ns / 1ps
module mxfp6_mult #(
    parameter int N = 32
)(
    input  logic [8+6*N-1:0] a,
    input  logic [8+6*N-1:0] b,
    output logic [8+6*N-1:0] out
);
    localparam int W = 8 + 6*N;

    logic [7:0]  scale1, scale2;
    logic signed [8:0] scale_sum, scale_out;

    logic       sign1    [0:N-1];
    logic       sign2    [0:N-1];
    logic       sign_out [0:N-1];
    logic [1:0] exp1     [0:N-1];
    logic [1:0] exp2     [0:N-1];
    logic [2:0] exp_sum  [0:N-1];   // sum of unbiased exponents, 0..4
    logic [3:0] mant1    [0:N-1];
    logic [3:0] mant2    [0:N-1];
    logic [7:0] product  [0:N-1];   // Q2.6: value = product / 64
    logic [2:0] lead     [0:N-1];   // position of leading one in product
    logic signed [4:0] e_res [0:N-1]; // unbiased result exponent
    logic [7:0]  norm    [0:N-1];   // product shifted so leading one is at bit 7
    logic [11:0] sub     [0:N-1];   // subnormal mantissa before slicing
    logic [1:0] exp_out  [0:N-1];
    logic [2:0] mant_out [0:N-1];

    always_comb begin
        scale1 = a[7:0];
        scale2 = b[7:0];

        scale_sum = 9'(scale1) + 9'(scale2);
        scale_out = scale_sum - 9'd127;
        out[7:0]  = scale_out[7:0];

        for (int k = 0; k < N; k++) begin
            // unpack
            sign1[k] = a[8 + 6*k + 5];
            sign2[k] = b[8 + 6*k + 5];
            exp1[k]  = a[8 + 6*k + 4 -: 2];
            exp2[k]  = b[8 + 6*k + 4 -: 2];

            // hidden bit: 1 for normal, 0 for subnormal
            mant1[k] = {(exp1[k] != 2'd0), a[8 + 6*k + 2 -: 3]};
            mant2[k] = {(exp2[k] != 2'd0), b[8 + 6*k + 2 -: 3]};

            sign_out[k] = sign1[k] ^ sign2[k];

            // unbiased exponents: normal = exp - 1, subnormal = 1 - 1 = 0
            exp_sum[k] = 3'((exp1[k] == 2'd0) ? 2'd0 : exp1[k] - 2'd1)
                       + 3'((exp2[k] == 2'd0) ? 2'd0 : exp2[k] - 2'd1);

            product[k] = mant1[k] * mant2[k];

            // leading-one detect (highest set bit wins)
            lead[k] = 3'd0;
            for (int p = 0; p < 8; p++) begin
                if (product[k][p]) lead[k] = 3'(p);
            end

            // leading one at bit 6 means value in [1,2), so exponent shifts by (lead - 6)
            e_res[k] = $signed({2'b00, exp_sum[k]}) + $signed({2'b00, lead[k]}) - 5'sd6;

            norm[k] = product[k] << (3'd7 - lead[k]);
            sub[k]  = ({4'b0000, product[k]} << exp_sum[k]) >> 3;

            if (product[k] == 8'd0) begin
                // zero operand
                exp_out[k]  = 2'd0;
                mant_out[k] = 3'd0;
            end
            else if (e_res[k] > 5'sd2) begin
                // overflow: saturate to max magnitude (1.875 * 2^2 = 7.5)
                exp_out[k]  = 2'd3;
                mant_out[k] = 3'b111;
            end
            else if (e_res[k] >= 5'sd0) begin
                // normal result
                exp_out[k]  = 2'(e_res[k] + 5'sd1);
                mant_out[k] = norm[k][6:4];
            end
            else begin
                // subnormal result: value = 0.mmm, so mmm = value * 8
                exp_out[k]  = 2'd0;
                mant_out[k] = sub[k][2:0];
            end

            // flush to +0 when the result rounds to zero
            if (exp_out[k] == 2'd0 && mant_out[k] == 3'd0)
                out[8 + 6*k + 5 -: 6] = 6'd0;
            else
                out[8 + 6*k + 5 -: 6] = {sign_out[k], exp_out[k], mant_out[k]};
        end
    end

endmodule
