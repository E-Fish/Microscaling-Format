# IVERILOG = iverilog
# FLAGS = -Wall -g2012

# # Standalone adder (used by the fp16/bf16/fp32 adder testbenches)
# FPA = fpa_rounding.sv

# # MAC unit and everything it depends on
# MAC_SRCS = mac.sv fpa_rounding.sv fp_mult.sv conv_comb.sv
# MAC_TB   = tb_mac.sv

# FP16ATB = tb_fp16a.sv
# BF16ATB = tb_bf16.sv
# FP32ATB = tb_fp32.sv
# MXFP6ATB = tb_mxfp6.sv

# OUT_FP16 = output_fp16
# OUT_BF16 = output_bf16
# OUT_FP32 = output_fp32
# OUT_MXFP6 = output_mxfp6
# OUT_MAC  = output_mac

# .PHONY: all mac clear

# all: $(OUT_FP16) $(OUT_BF16) $(OUT_FP32) $(OUT_MAC)
# 	./$(OUT_FP16)
# 	./$(OUT_BF16)
# 	./$(OUT_FP32)
# 	./$(OUT_MAC)

# $(OUT_FP16): $(FPA) $(FP16ATB)
# 	$(IVERILOG) $(FLAGS) -o $(OUT_FP16) $(FPA) $(FP16ATB)

# $(OUT_BF16): $(FPA) $(BF16ATB)
# 	$(IVERILOG) $(FLAGS) -o $(OUT_BF16) $(FPA) $(BF16ATB)

# $(OUT_FP32): $(FPA) $(FP32ATB)
# 	$(IVERILOG) $(FLAGS) -o $(OUT_FP32) $(FPA) $(FP32ATB)

# $(OUT_MXFP6): $(FPA) $(MXFP6ATB)
# 	$(IVERILOG) $(FLAGS) -o $(OUT_MXFP6) $(FPA) $(MXFP6ATB)

# $(OUT_MAC): $(MAC_SRCS) $(MAC_TB)
# 	$(IVERILOG) $(FLAGS) -o $(OUT_MAC) $(MAC_SRCS) $(MAC_TB)

# # Run just the MAC test on its own
# mac: $(OUT_MAC)
# 	./$(OUT_MAC)

# clear:
# 	rm -f $(OUT_FP16) $(OUT_BF16) $(OUT_FP32) $(OUT_MAC) waveform.vcd
	

	IVERILOG = iverilog
FLAGS = -Wall -g2012

# Standalone adder (used by the fp16/bf16/fp32 adder testbenches)
FPA = fpa_rounding.sv

# MXFP6 multiplier and its testbench
MXFP6_SRCS = mxfp6_mult.sv
MXFP6ATB   = tb_mxfp6_mult.sv

# MAC unit and everything it depends on
MAC_SRCS = mac.sv fpa_rounding.sv fp_mult.sv conv_comb.sv
MAC_TB   = tb_mac.sv

FP16ATB = tb_fp16a.sv
BF16ATB = tb_bf16.sv
FP32ATB = tb_fp32.sv

OUT_FP16  = output_fp16
OUT_BF16  = output_bf16
OUT_FP32  = output_fp32
OUT_MXFP6 = output_mxfp6
OUT_MAC   = output_mac

.PHONY: all mac mxfp6 clear

all: $(OUT_FP16) $(OUT_BF16) $(OUT_FP32) $(OUT_MXFP6) $(OUT_MAC)
	./$(OUT_FP16)
	./$(OUT_BF16)
	./$(OUT_FP32)
	./$(OUT_MXFP6)
	./$(OUT_MAC)

$(OUT_FP16): $(FPA) $(FP16ATB)
	$(IVERILOG) $(FLAGS) -o $(OUT_FP16) $(FPA) $(FP16ATB)

$(OUT_BF16): $(FPA) $(BF16ATB)
	$(IVERILOG) $(FLAGS) -o $(OUT_BF16) $(FPA) $(BF16ATB)

$(OUT_FP32): $(FPA) $(FP32ATB)
	$(IVERILOG) $(FLAGS) -o $(OUT_FP32) $(FPA) $(FP32ATB)

$(OUT_MXFP6): $(MXFP6_SRCS) $(MXFP6ATB)
	$(IVERILOG) $(FLAGS) -o $(OUT_MXFP6) $(MXFP6_SRCS) $(MXFP6ATB)

$(OUT_MAC): $(MAC_SRCS) $(MAC_TB)
	$(IVERILOG) $(FLAGS) -o $(OUT_MAC) $(MAC_SRCS) $(MAC_TB)

mxfp6: $(OUT_MXFP6)
	./$(OUT_MXFP6)

mac: $(OUT_MAC)
	./$(OUT_MAC)

clear:
	rm -f $(OUT_FP16) $(OUT_BF16) $(OUT_FP32) $(OUT_MXFP6) $(OUT_MAC) waveform.vcd