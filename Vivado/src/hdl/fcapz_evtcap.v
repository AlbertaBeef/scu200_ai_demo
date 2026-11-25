// fcapz_evtcap -- event-timeline capture for scu200_ai_demo03_fcapz.
//
// Taps (read-only, AXI "monitor" interfaces) the camera frame-buffer writer, the
// display frame-buffer reader and mbdpu03's m_axi read channel, reduces each to a
// "busy" level plus the frame slot (0..2, 3 = not a camera slot) it is touching,
// adds the FSIN strobe, the two frame-buffer interrupts and 16 firmware marker bits,
// and feeds a 32-bit event word to an fpgacapZero ELA (4K deep, 32-bit timestamps).
//
// Only samples where something changed are stored: bit 0 (evt) is 1 for exactly the
// sample in which any other bit changed, and the ELA's storage qualifier is set to
// "store when bit0 == 1". Each stored entry is one event, time-stamped in 100 MHz
// cycles, so 4K entries cover seconds of activity.
//
// Event word (probe file: fcapz_evtcap.prob):
//   [0]     evt        any of [31:1] changed this sample
//   [1]     fsin       disc_strobe XVS -> OV9281 FSIN
//   [2]     wr_busy    frmbuf_wr wrote a camera slot within the last WR_HOLD cycles (300 MHz)
//   [4:3]   wr_slot    slot of the last frmbuf_wr write burst
//   [5]     wr_irq     frmbuf_wr interrupt line
//   [6]     dpu_busy   mbdpu03 read a camera slot within the last DPU_HOLD cycles
//   [8:7]   dpu_slot   slot of the last mbdpu03 camera read burst
//   [9]     rd_irq     frmbuf_rd (display) interrupt line
//   [11:10] disp_slot  slot of the last display read burst
//   [17:12] mark_a     firmware main-loop markers (fcapz_mark GPIO ch1 [5:0])
//   [23:18] mark_b     firmware ISR markers       (fcapz_mark GPIO ch2 [5:0])
//   [24]    col_dpu    dpu_busy & wr_busy & wr_slot == dpu_slot   (input tearing)
//   [25]    col_disp   wr_busy & wr_slot == disp_slot              (display tearing)
//   [27:26] mark_a[7:6]
//   [29:28] mark_b[7:6]
//   [31:30] 0
//
// Clocks: clk = 100 MHz (sample clock, mbdpu03, disc_strobe, GPIO); vclk = 300 MHz
// (frame buffers). Both come from the same clk_wiz, so the 300->100 crossings are
// timed; the 2-FF stages are kept anyway (ASYNC_REG) and slot buses are only taken
// once two consecutive samples agree.
//
// The ELA sits on BSCANE2 USER3 (MDM = USER2, Vivado dbg_hub = USER1).

`timescale 1ns/1ps

module fcapz_evtcap #(
    parameter [31:0] SLOT_BASE  = 32'hA0000000,  // CAM0_FRMBUF_BUFR_ADDR
    parameter [31:0] SLOT_SIZE  = 32'h000E1000,  // 640 x 480 x RGB8 (stride 1920)
    parameter        WR_HOLD_W  = 15,            // 2^15 vclk = 109 us busy hold
    parameter        RD_HOLD_W  = 15,
    parameter        DPU_HOLD_W = 12,            // 2^12 clk  =  41 us busy hold
    parameter        DEPTH      = 4096,
    parameter        CTRL_CHAIN = 3
) (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF MON_DPU, ASSOCIATED_RESET aresetn" *)
    input  wire        clk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire        aresetn,
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 vclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF MON_WR:MON_RD" *)
    input  wire        vclk,

    // frmbuf_wr write-address channel (monitor)
    (* X_INTERFACE_MODE = "monitor" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_WR AWADDR" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME MON_WR, PROTOCOL AXI4, READ_WRITE_MODE READ_WRITE" *)
    input  wire [31:0] mon_wr_awaddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_WR AWVALID" *)
    input  wire        mon_wr_awvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_WR AWREADY" *)
    input  wire        mon_wr_awready,
    // (W/B channel ports only so IPI infers a complete write interface; unused)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_WR WDATA" *)
    input  wire [127:0] mon_wr_wdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_WR WVALID" *)
    input  wire         mon_wr_wvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_WR WREADY" *)
    input  wire         mon_wr_wready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_WR BVALID" *)
    input  wire         mon_wr_bvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_WR BREADY" *)
    input  wire         mon_wr_bready,

    // display frmbuf_rd read-address channel (monitor)
    (* X_INTERFACE_MODE = "monitor" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_RD ARADDR" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME MON_RD, PROTOCOL AXI4, READ_WRITE_MODE READ_WRITE" *)
    input  wire [31:0] mon_rd_araddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_RD ARVALID" *)
    input  wire        mon_rd_arvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_RD ARREADY" *)
    input  wire        mon_rd_arready,
    // (R channel ports only so IPI infers a complete read interface; unused)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_RD RDATA" *)
    input  wire [127:0] mon_rd_rdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_RD RVALID" *)
    input  wire         mon_rd_rvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_RD RREADY" *)
    input  wire         mon_rd_rready,

    // mbdpu03 m_axi_gmem read-address channel (monitor)
    (* X_INTERFACE_MODE = "monitor" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_DPU ARADDR" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME MON_DPU, PROTOCOL AXI4, READ_WRITE_MODE READ_WRITE, ID_WIDTH 1" *)
    input  wire [63:0] mon_dpu_araddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_DPU ARVALID" *)
    input  wire        mon_dpu_arvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_DPU ARREADY" *)
    input  wire        mon_dpu_arready,
    // (R channel ports only so IPI infers a complete read interface; unused)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_DPU RDATA" *)
    input  wire [63:0]  mon_dpu_rdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_DPU RVALID" *)
    input  wire         mon_dpu_rvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 MON_DPU RREADY" *)
    input  wire         mon_dpu_rready,

    input  wire        fsin,      // clk domain
    input  wire        wr_irq,    // vclk domain
    input  wire        rd_irq,    // vclk domain
    input  wire [7:0]  mark_a,    // clk domain
    input  wire [7:0]  mark_b     // clk domain
);

    // ------------------------------------------------------------------ slot decode
    // 3 = outside the three camera slots.
    function [1:0] slot_of(input [31:0] a);
        reg [31:0] off;
        begin
            off = a - SLOT_BASE;
            if (a < SLOT_BASE)              slot_of = 2'd3;
            else if (off < SLOT_SIZE)       slot_of = 2'd0;
            else if (off < 2 * SLOT_SIZE)   slot_of = 2'd1;
            else if (off < 3 * SLOT_SIZE)   slot_of = 2'd2;
            else                            slot_of = 2'd3;
        end
    endfunction

    // One AXI address channel -> {busy, slot}. Stage 1 registers the handshake and
    // address, stage 2 decodes, so the 32-bit compares are off the bus timing.
    // Only bursts into a camera slot count; slot holds its last camera value.

    // ---- frmbuf_wr (vclk)
    reg        wr_hs1;  reg [31:0] wr_a1;
    reg [1:0]  wr_slot_v = 2'd3;
    reg [WR_HOLD_W:0] wr_cnt = 0;   // MSB = busy
    always @(posedge vclk) begin
        wr_hs1 <= mon_wr_awvalid & mon_wr_awready;
        wr_a1  <= mon_wr_awaddr;
        if (wr_hs1 && slot_of(wr_a1) != 2'd3) begin
            wr_slot_v <= slot_of(wr_a1);
            wr_cnt    <= {1'b1, {WR_HOLD_W{1'b1}}};
        end else if (wr_cnt[WR_HOLD_W]) begin
            wr_cnt    <= wr_cnt - 1'b1;     // busy drops when the MSB clears
        end
    end

    // ---- display frmbuf_rd (vclk)
    reg        rd_hs1;  reg [31:0] rd_a1;
    reg [1:0]  rd_slot_v = 2'd3;
    always @(posedge vclk) begin
        rd_hs1 <= mon_rd_arvalid & mon_rd_arready;
        rd_a1  <= mon_rd_araddr;
        if (rd_hs1 && slot_of(rd_a1) != 2'd3)
            rd_slot_v <= slot_of(rd_a1);
    end

    // ---- mbdpu03 (clk)
    reg        dpu_hs1; reg [31:0] dpu_a1;
    reg [1:0]  dpu_slot = 2'd3;
    reg [DPU_HOLD_W:0] dpu_cnt = 0;
    always @(posedge clk) begin
        dpu_hs1 <= mon_dpu_arvalid & mon_dpu_arready;
        dpu_a1  <= mon_dpu_araddr[31:0];
        if (dpu_hs1 && slot_of(dpu_a1) != 2'd3) begin
            dpu_slot <= slot_of(dpu_a1);
            dpu_cnt  <= {1'b1, {DPU_HOLD_W{1'b1}}};
        end else if (dpu_cnt[DPU_HOLD_W]) begin
            dpu_cnt  <= dpu_cnt - 1'b1;
        end
    end

    // ---- vclk -> clk: {wr_busy, wr_slot[2], rd_slot[2], wr_irq, rd_irq}
    reg [6:0] v_q;   // source-registered (vclk)
    always @(posedge vclk)
        v_q <= {rd_irq, wr_irq, rd_slot_v, wr_slot_v, wr_cnt[WR_HOLD_W]};
    (* ASYNC_REG = "TRUE" *) reg [6:0] s1, s2;
    reg [6:0] s3;
    always @(posedge clk) begin
        s1 <= v_q;
        s2 <= s1;
        s3 <= s2;
    end
    // Groups are taken only when stable for two samples. {wr_busy, wr_slot} move as one
    // group so busy never rises against a stale slot (that gave false collisions).
    reg [2:0] wr_grp = 3'b110;      // {slot=3, busy=0}
    reg [1:0] disp_slot = 2'd3;
    reg [1:0] irq_s = 2'b00;
    always @(posedge clk) begin
        if (s2[2:0] == s3[2:0]) wr_grp    <= s2[2:0];
        if (s2[4:3] == s3[4:3]) disp_slot <= s2[4:3];
        irq_s <= s2[6:5];            // match the filter's one-sample delay
    end
    wire       wr_busy  = wr_grp[0];
    wire [1:0] wr_slot  = wr_grp[2:1];
    wire       wr_irq_s = irq_s[0];
    wire       rd_irq_s = irq_s[1];

    // ------------------------------------------------------------------ event word
    reg  [7:0] ma_q, mb_q;
    reg        fsin_q;
    always @(posedge clk) begin
        ma_q   <= mark_a;
        mb_q   <= mark_b;
        fsin_q <= fsin;
    end
    wire dpu_busy = dpu_cnt[DPU_HOLD_W];
    wire col_dpu  = dpu_busy & wr_busy & (wr_slot == dpu_slot);
    wire col_disp = wr_busy & (wr_slot == disp_slot);

    wire [31:1] cur = {2'b00, mb_q[7:6], ma_q[7:6], col_disp, col_dpu,
                       mb_q[5:0], ma_q[5:0], disp_slot, rd_irq_s, dpu_slot, dpu_busy,
                       wr_irq_s, wr_slot, wr_busy, fsin_q};
    reg  [31:1] cur_q = 0;
    reg         evt   = 1'b0;
    always @(posedge clk) begin
        cur_q <= cur;
        evt   <= (cur != cur_q);   // flags the sample in which cur_q takes the new value
    end
    // evt(t) = cur(t-1) != cur(t-2) = cur_q(t) != cur_q(t-1): aligned with cur_q.
    wire [31:0] probe = {cur_q, evt};

    // ------------------------------------------------------------------ ELA
    fcapz_ela_xilinxus #(
        .SAMPLE_W     (32),
        .DEPTH        (DEPTH),
        .TRIG_STAGES  (1),
        .STOR_QUAL    (1),
        .INPUT_PIPE   (1),
        .TIMESTAMP_W  (32),
        .REL_COMPARE  (0),
        .DUAL_COMPARE (1),
        .CTRL_CHAIN   (CTRL_CHAIN),
        .SINGLE_CHAIN_BURST (1)
    ) u_ela (
        .sample_clk    (clk),
        .sample_rst    (~aresetn),
        .probe_in      (probe),
        .trigger_in    (1'b0),
        .trigger_out   (),
        .armed_out     (),
        .eio_probe_in  (1'b0),
        .eio_probe_out ()
    );

endmodule
