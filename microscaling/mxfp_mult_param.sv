// Parameterized MX (OCP Microscaling) block multiplier
//
// Block layout: [7:0] shared E8M0 scale, then N elements of (1 + EXP + MAN) bits,
// element k at bits [8 + k*ELEM +: ELEM], each laid out as {sign, exponent, mantissa}.
//
// Supported element formats (set EXP / MAN):
//   MXFP4  E2M1 : EXP=2, MAN=1   bias 1,  no inf/NaN, max 6.0
//   MXFP6  E2M3 : EXP=2, MAN=3   bias 1,  no inf/NaN, max 7.5
//   MXFP6  E3M2 : EXP=3, MAN=2   bias 3,  no inf/NaN, max 28.0
//   MXFP8  E4M3 : EXP=4, MAN=3   bias 7,  NaN = S.1111.111, no inf, max 448
//   MXFP8  E5M2 : EXP=5, MAN=2   bias 15, inf/NaN on exp=11111, max 57344
//
// Arithmetic: products are truncated (round toward zero), overflow saturates to
// +/- max finite, results that truncate to zero are flushed to +0.
// FP8 specials: any NaN input, or inf * 0, gives NaN ({0, all ones}); inf * x gives inf.

`timescale 1ns / 1ps

module mx_mult #(
    parameter int EXP = 2,
    parameter int MAN = 3,
    parameter int N   = 32
)(
    input  logic [8+N*(1+EXP+MAN)-1:0] a,
    input  logic [8+N*(1+EXP+MAN)-1:0] b,
    output logic [8+N*(1+EXP+MAN)-1:0] out
);
    localparam int ELEM     = 1 + EXP + MAN;           // element width
    localparam int BIAS     = (1 << (EXP-1)) - 1;
    localparam int EMIN     = 1 - BIAS;                // unbiased exponent of subnormals / min normal
    localparam bit IS_E5M2  = (EXP == 5 && MAN == 2);  // IEEE-style inf/NaN
    localparam bit IS_E4M3  = (EXP == 4 && MAN == 3);  // NaN only
    localparam int EXP_ALL1 = (1 << EXP) - 1;
    localparam int MAXB     = IS_E5M2 ? EXP_ALL1 - 1 : EXP_ALL1;          // max biased exp of a finite value
    localparam int MAXM     = IS_E4M3 ? (1 << MAN) - 2 : (1 << MAN) - 1;  // max mantissa at MAXB
    localparam int PW       = 2 * (MAN + 1);           // mantissa product width
    localparam int LW       = $clog2(PW);              // leading-one index width
    localparam int XW       = EXP + LW + 3;            // signed exponent math width

    // ---------------- shared scale (E8M0) ----------------
    logic [8:0] scale_sum;
    assign scale_sum = 9'(a[7:0]) + 9'(b[7:0]) - 9'd127;
    assign out[7:0]  = scale_sum[7:0];

    // ---------------- per-element lanes ----------------
    for (genvar k = 0; k < N; k++) begin : g_lane
        logic [ELEM-1:0] ea, eb, eo;

        logic                 s;
        logic [EXP-1:0]       xa, xb;
        logic [MAN-1:0]       ma, mb;
        logic                 a_zero, b_zero, a_inf, b_inf, a_nan, b_nan;
        logic [MAN:0]         fa, fb;          // mantissas with hidden bit
        logic [PW-1:0]        p;               // value = p / 2^(2*MAN) * 2^(ta+tb)
        logic [LW-1:0]        lead;            // index of leading one in p
        logic signed [XW-1:0] ta, tb, es, er, sh;
        logic [XW-1:0]        rsh;
        logic [PW-1:0]        norm, subm;
        logic [MAN-1:0]       mo_norm, mo_sub;

        assign ea = a[8 + k*ELEM +: ELEM];
        assign eb = b[8 + k*ELEM +: ELEM];
        assign out[8 + k*ELEM +: ELEM] = eo;

        always_comb begin
            // unpack
            s  = ea[ELEM-1] ^ eb[ELEM-1];
            xa = ea[MAN +: EXP];
            xb = eb[MAN +: EXP];
            ma = ea[MAN-1:0];
            mb = eb[MAN-1:0];

            a_zero = (xa == '0) && (ma == '0);
            b_zero = (xb == '0) && (mb == '0);

            // special encodings (only exist for the FP8 formats)
            a_nan = (IS_E5M2 && xa == EXP'(EXP_ALL1) && ma != '0) ||
                    (IS_E4M3 && xa == EXP'(EXP_ALL1) && ma == '1);
            b_nan = (IS_E5M2 && xb == EXP'(EXP_ALL1) && mb != '0) ||
                    (IS_E4M3 && xb == EXP'(EXP_ALL1) && mb == '1);
            a_inf = IS_E5M2 && xa == EXP'(EXP_ALL1) && ma == '0;
            b_inf = IS_E5M2 && xb == EXP'(EXP_ALL1) && mb == '0;

            // hidden bit: 1 for normal, 0 for subnormal
            fa = {(xa != '0), ma};
            fb = {(xb != '0), mb};

            // unbiased exponents: normal = exp - BIAS, subnormal = EMIN
            ta = (xa == '0) ? XW'(EMIN) : XW'(xa) - XW'(BIAS);
            tb = (xb == '0) ? XW'(EMIN) : XW'(xb) - XW'(BIAS);
            es = ta + tb;

            p = PW'(fa) * PW'(fb);

            // leading-one detect (highest set bit wins)
            lead = '0;
            for (int i = 0; i < PW; i++) begin
                if (p[i]) lead = LW'(i);
            end

            // leading one at bit 2*MAN means value in [1,2)
            er = es + XW'(lead) - XW'(2*MAN);

            // normal path: shift leading one to the top, take the next MAN bits
            norm    = p << (LW'(PW-1) - lead);
            mo_norm = norm[PW-2 -: MAN];

            // subnormal path: mantissa = value * 2^(MAN - EMIN) = p * 2^(es - EMIN - MAN)
            sh   = es - XW'(EMIN) - XW'(MAN);
            rsh  = -sh;
            subm = sh[XW-1] ? (p >> rsh) : (p << sh);
            mo_sub = subm[MAN-1:0];

            if (a_nan || b_nan || (a_inf && b_zero) || (b_inf && a_zero)) begin
                eo = {1'b0, {EXP{1'b1}}, {MAN{1'b1}}};          // NaN
            end
            else if (a_inf || b_inf) begin
                eo = {s, {EXP{1'b1}}, {MAN{1'b0}}};             // inf
            end
            else if (p == '0) begin
                eo = '0;                                        // zero operand
            end
            else if ($signed(er) >= $signed(XW'(EMIN))) begin
                // normal result, saturate if past max finite
                if ($signed(er) + $signed(XW'(BIAS)) > $signed(XW'(MAXB)) ||
                    ($signed(er) + $signed(XW'(BIAS)) == $signed(XW'(MAXB)) && mo_norm > MAN'(MAXM)))
                    eo = {s, EXP'(MAXB), MAN'(MAXM)};
                else
                    eo = {s, EXP'(er + XW'(BIAS)), mo_norm};
            end
            else begin
                // subnormal result, flush to +0 if it truncates to zero
                if (mo_sub == '0)
                    eo = '0;
                else
                    eo = {s, {EXP{1'b0}}, mo_sub};
            end
        end
    end

endmodule
