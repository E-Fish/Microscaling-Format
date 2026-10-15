`timescale 1ns / 1ps

module tb_mxfp6_mult;

    localparam int NLANES = 32;
    localparam int EW = 6;      //bits per fp6 element
    localparam int SW = 8;      //bits for the e8m0 block scale
    localparam int TOTAL = SW + NLANES*EW;

    logic [TOTAL-1:0] a, b, out;

    mxfp6_mult dut (
        .a(a),
        .b(b),
        .out(out)
    );

    int pass_count = 0;
    int fail_count = 0;

    logic [EW-1:0] a_elems [NLANES];
    logic [EW-1:0] b_elems [NLANES];
    logic [EW-1:0] e_elems [NLANES];
    logic [TOTAL-1:0] blk_a, blk_b, blk_e;

    //pack one fp6 element: sign, 2-bit exp, 3-bit mant
    function automatic logic [EW-1:0] fp6(logic s, logic [1:0] e, logic [2:0] m);
        fp6 = {s, e, m};
    endfunction

    task automatic clear_elems();
        for (int i = 0; i < NLANES; i++) begin
            a_elems[i] = '0;
            b_elems[i] = '0;
            e_elems[i] = '0;
        end
    endtask

    //icarus doesn't take unpacked arrays as task ports, so these just
    //read a_elems/b_elems/e_elems directly instead of taking the array in
    task automatic build_block_a(input logic [SW-1:0] scale_f, output logic [TOTAL-1:0] blk);
        blk = '0;
        blk[SW-1:0] = scale_f;
        for (int i = 0; i < NLANES; i++) begin
            blk[SW + EW*i +: EW] = a_elems[i];
        end
    endtask

    task automatic build_block_b(input logic [SW-1:0] scale_f, output logic [TOTAL-1:0] blk);
        blk = '0;
        blk[SW-1:0] = scale_f;
        for (int i = 0; i < NLANES; i++) begin
            blk[SW + EW*i +: EW] = b_elems[i];
        end
    endtask

    task automatic build_block_e(input logic [SW-1:0] scale_f, output logic [TOTAL-1:0] blk);
        blk = '0;
        blk[SW-1:0] = scale_f;
        for (int i = 0; i < NLANES; i++) begin
            blk[SW + EW*i +: EW] = e_elems[i];
        end
    endtask

    task run_test(logic [TOTAL-1:0] test_a, logic [TOTAL-1:0] test_b, logic [TOTAL-1:0] expected);
        begin
            a = test_a;
            b = test_b;

            #1;

            if (out === expected) begin
                $display("a=%h b=%h out=%h PASS", test_a, test_b, out);
                pass_count++;
            end
            else begin
                $display("a=%h b=%h out=%h expected=%h FAIL", test_a, test_b, out, expected);
                fail_count++;
            end
        end
    endtask

    initial begin
        $dumpfile("waveform.vcd");
        $dumpvars(0, tb_mxfp6_mult);

        a = '0;
        b = '0;
        #1;

        //all zero blocks, neutral scale on both sides
        clear_elems();
        build_block_a(8'd127, blk_a);
        build_block_b(8'd127, blk_b);
        build_block_e(8'd127, blk_e);
        run_test(blk_a, blk_b, blk_e);

        //lane0: 0.0 * 1.0 = 0.0
        clear_elems();
        a_elems[0] = fp6(1'b0, 2'b00, 3'b000);
        b_elems[0] = fp6(1'b0, 2'b01, 3'b000);
        build_block_a(8'd127, blk_a);
        build_block_b(8'd127, blk_b);
        build_block_e(8'd127, blk_e);      //lane 0 stays zero
        run_test(blk_a, blk_b, blk_e);

        //lane0: 1.0 * 1.0 = 1.0, no-shift normalization branch
        clear_elems();
        a_elems[0] = fp6(1'b0, 2'b01, 3'b000);
        b_elems[0] = fp6(1'b0, 2'b01, 3'b000);
        e_elems[0] = fp6(1'b0, 2'b01, 3'b000);
        build_block_a(8'd127, blk_a);
        build_block_b(8'd127, blk_b);
        build_block_e(8'd127, blk_e);
        run_test(blk_a, blk_b, blk_e);

        //lane0: 1.75 * 1.75 -> truncates to 3.0, shift normalization branch
        clear_elems();
        a_elems[0] = fp6(1'b0, 2'b01, 3'b110);
        b_elems[0] = fp6(1'b0, 2'b01, 3'b110);
        e_elems[0] = fp6(1'b0, 2'b10, 3'b100);
        build_block_a(8'd127, blk_a);
        build_block_b(8'd127, blk_b);
        build_block_e(8'd127, blk_e);
        run_test(blk_a, blk_b, blk_e);

        //lane0: -1.5 * 1.5 = -2.25, lane1: 2.0 * 2.0 = 4.0 (sign handling)
        clear_elems();
        a_elems[0] = fp6(1'b1, 2'b01, 3'b100);
        b_elems[0] = fp6(1'b0, 2'b01, 3'b100);
        e_elems[0] = fp6(1'b1, 2'b10, 3'b001);
        a_elems[1] = fp6(1'b0, 2'b10, 3'b000);
        b_elems[1] = fp6(1'b0, 2'b10, 3'b000);
        e_elems[1] = fp6(1'b0, 2'b11, 3'b000);
        build_block_a(8'd127, blk_a);
        build_block_b(8'd127, blk_b);
        build_block_e(8'd127, blk_e);
        run_test(blk_a, blk_b, blk_e);

        //known limitation: lane0: 0.75 * 4.0 -> module only does a single-bit
        //normalization shift, wrong once an operand is subnormal, expect FAIL
        clear_elems();
        a_elems[0] = fp6(1'b0, 2'b00, 3'b110);
        b_elems[0] = fp6(1'b0, 2'b11, 3'b000);
        e_elems[0] = fp6(1'b0, 2'b10, 3'b100);
        build_block_a(8'd127, blk_a);
        build_block_b(8'd127, blk_b);
        build_block_e(8'd127, blk_e);
        run_test(blk_a, blk_b, blk_e);

        //known limitation: lane0: 0.5 * 0.25 -> true result needs a subnormal
        //output, module never produces one, expect FAIL
        clear_elems();
        a_elems[0] = fp6(1'b0, 2'b00, 3'b100);
        b_elems[0] = fp6(1'b0, 2'b00, 3'b010);
        e_elems[0] = fp6(1'b0, 2'b00, 3'b001);
        build_block_a(8'd127, blk_a);
        build_block_b(8'd127, blk_b);
        build_block_e(8'd127, blk_e);
        run_test(blk_a, blk_b, blk_e);

        //scale combine only, elements all zero: 130 + 120 - 127 = 123
        clear_elems();
        build_block_a(8'd130, blk_a);
        build_block_b(8'd120, blk_b);
        build_block_e(8'd123, blk_e);
        run_test(blk_a, blk_b, blk_e);

        //scale should not get folded into the element field too:
        //140 + 100 - 127 = 113, lane3 1.0*1.0 stays 1.0 regardless
        clear_elems();
        a_elems[3] = fp6(1'b0, 2'b01, 3'b000);
        b_elems[3] = fp6(1'b0, 2'b01, 3'b000);
        e_elems[3] = fp6(1'b0, 2'b01, 3'b000);
        build_block_a(8'd140, blk_a);
        build_block_b(8'd100, blk_b);
        build_block_e(8'd113, blk_e);
        run_test(blk_a, blk_b, blk_e);

        //multi-lane independence: lane0 1.0*1.0=1.0, lane5 2.0*2.0=4.0,
        //lane10 -1.5*1.5=-2.25, everything else stays zero
        clear_elems();
        a_elems[0]  = fp6(1'b0, 2'b01, 3'b000);
        b_elems[0]  = fp6(1'b0, 2'b01, 3'b000);
        e_elems[0]  = fp6(1'b0, 2'b01, 3'b000);
        a_elems[5]  = fp6(1'b0, 2'b10, 3'b000);
        b_elems[5]  = fp6(1'b0, 2'b10, 3'b000);
        e_elems[5]  = fp6(1'b0, 2'b11, 3'b000);
        a_elems[10] = fp6(1'b1, 2'b01, 3'b100);
        b_elems[10] = fp6(1'b0, 2'b01, 3'b100);
        e_elems[10] = fp6(1'b1, 2'b10, 3'b001);
        build_block_a(8'd127, blk_a);
        build_block_b(8'd127, blk_b);
        build_block_e(8'd127, blk_e);
        run_test(blk_a, blk_b, blk_e);

        //full block stress: every lane 1.0 * 1.0 = 1.0
        for (int i = 0; i < NLANES; i++) begin
            a_elems[i] = fp6(1'b0, 2'b01, 3'b000);
            b_elems[i] = fp6(1'b0, 2'b01, 3'b000);
            e_elems[i] = fp6(1'b0, 2'b01, 3'b000);
        end
        build_block_a(8'd127, blk_a);
        build_block_b(8'd127, blk_b);
        build_block_e(8'd127, blk_e);
        run_test(blk_a, blk_b, blk_e);

        $display("pass: %0d fail: %0d", pass_count, fail_count);

        $finish;
    end

endmodule