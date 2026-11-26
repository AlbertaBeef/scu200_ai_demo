// fsin_frame_tag.v
//
// Sits on the AXI4-Stream video path in front of frmbuf_wr.
//  * Times every frame's start-of-frame (tuser) relative to the last FSIN.
//  * Latches that delay, an FSIN counter and a frame-in-strobe index into
//    registers that software reads over the AXI4-Lite port below.
//  * Optional (CTRL.drop_en = 1): discards any frame whose SOF arrives less
//    than the drop window after FSIN, so frmbuf_wr only ever sees the good
//    frame and no dummy buffer is needed.
//
// Classification is by time since FSIN, not by counting frames, so a
// missing first frame doesn't cause the good frame to be dropped.
//
// From the fcapz trace: first frame SOF ~1.06 ms after FSIN, second ~26.05 ms
// (one sensor frame, VTS lines, later). A window of ~13 ms sits between them.
// Example: 150 MHz -> 1_950_000 cycles. scu200_ai_demo03 runs it on the 300 MHz
// video clock (DROP_WINDOW = 3_900_000); the firmware moves the window through
// DROP_WINDOW register when it shortens the sensor frame (VTS).
//
// AXI4-Lite register map (aclk domain, 32-bit, byte offsets):
//   0x00 CTRL        RW  [0] drop_en   [1] clear counters (write 1, self-clearing)
//   0x04 DROP_WINDOW RW  window in aclk cycles; 0 = DROP_WINDOW parameter
//   0x08 FSIN_COUNT  RO  FSIN rising edges seen
//   0x0C DROPPED     RO  frames discarded by the gate
//   0x10 SOF_DELAY   RO  cycles FSIN -> SOF of the latest frame
//   0x14 LAST_FRAME  RO  [7:0] frame index in strobe (0 = first after FSIN), [8] early
//   0x18 FRAME_FSIN  RO  FSIN_COUNT at the latest SOF
//   0x1C ID          RO  0x46544101 ("FTA", version 1)

module fsin_frame_tag #(
    parameter integer TDATA_W     = 24,
    parameter integer DROP_WINDOW = 1_950_000   // cycles of aclk (default window)
)(
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axis:m_axis:S_AXI, ASSOCIATED_RESET aresetn" *)
    input  wire                 aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire                 aresetn,
    input  wire                 fsin_async,     // FSIN strobe, active high

    // from camera pipeline
    input  wire [TDATA_W-1:0]   s_axis_tdata,
    input  wire                 s_axis_tvalid,
    output wire                 s_axis_tready,
    input  wire                 s_axis_tuser,   // SOF
    input  wire                 s_axis_tlast,   // EOL

    // to frmbuf_wr
    output wire [TDATA_W-1:0]   m_axis_tdata,
    output wire                 m_axis_tvalid,
    input  wire                 m_axis_tready,
    output wire                 m_axis_tuser,
    output wire                 m_axis_tlast,

    // AXI4-Lite slave (register map above)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWADDR" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME S_AXI, PROTOCOL AXI4LITE, DATA_WIDTH 32, ADDR_WIDTH 5" *)
    input  wire [4:0]           s_axi_awaddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWVALID" *)
    input  wire                 s_axi_awvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWREADY" *)
    output reg                  s_axi_awready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WDATA" *)
    input  wire [31:0]          s_axi_wdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WSTRB" *)
    input  wire [3:0]           s_axi_wstrb,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WVALID" *)
    input  wire                 s_axi_wvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WREADY" *)
    output reg                  s_axi_wready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BRESP" *)
    output wire [1:0]           s_axi_bresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BVALID" *)
    output reg                  s_axi_bvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BREADY" *)
    input  wire                 s_axi_bready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARADDR" *)
    input  wire [4:0]           s_axi_araddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARVALID" *)
    input  wire                 s_axi_arvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARREADY" *)
    output reg                  s_axi_arready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RDATA" *)
    output reg  [31:0]          s_axi_rdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RRESP" *)
    output wire [1:0]           s_axi_rresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RVALID" *)
    output reg                  s_axi_rvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RREADY" *)
    input  wire                 s_axi_rready,

    // status, also as ports (e.g. for a logic analyser)
    output reg  [31:0]          fsin_count,       // FSIN edges seen
    output reg  [31:0]          last_sof_delay,   // cycles FSIN->SOF, latest frame
    output reg  [7:0]           last_frame_idx,   // 0 = first frame after FSIN
    output reg  [31:0]          last_frame_fsin,  // fsin_count at that SOF
    output reg                  last_frame_early, // 1 = classified as dummy
    output reg  [31:0]          dropped_count
);

    // ---- control registers (AXI4-Lite) ------------------------------------
    reg        drop_en;
    reg [31:0] drop_window;     // 0 = use DROP_WINDOW
    reg        clr_cnt;         // one-cycle pulse

    // ---- FSIN synchroniser + rising-edge detect --------------------------
    (* ASYNC_REG = "TRUE" *) reg [1:0] fsin_meta;
    reg       fsin_d;
    always @(posedge aclk) begin
        fsin_meta <= {fsin_meta[0], fsin_async};
        fsin_d    <= fsin_meta[1];
    end
    wire fsin_rise = fsin_meta[1] & ~fsin_d;

    // ---- time since FSIN (saturating) and frame index --------------------
    reg [31:0] since_fsin;
    reg [7:0]  frame_idx;

    wire sof_beat = s_axis_tvalid & s_axis_tuser & s_axis_tready;
    reg  [31:0] win;
    always @(posedge aclk)
        win <= (drop_window != 32'd0) ? drop_window : DROP_WINDOW;
    wire early    = (since_fsin < win);

    always @(posedge aclk) begin
        if (!aresetn) begin
            since_fsin <= 32'hFFFF_FFFF;   // "long ago" until first FSIN
            frame_idx  <= 8'd0;
            fsin_count <= 32'd0;
        end else begin
            if (fsin_rise) begin
                since_fsin <= 32'd0;
                frame_idx  <= 8'd0;
                fsin_count <= fsin_count + 1;
            end else begin
                if (since_fsin != 32'hFFFF_FFFF) since_fsin <= since_fsin + 1;
                if (sof_beat && frame_idx != 8'hFF) frame_idx <= frame_idx + 1;
            end
            if (clr_cnt) fsin_count <= 32'd0;
        end
    end

    // ---- latch tag at each SOF -------------------------------------------
    always @(posedge aclk) begin
        if (!aresetn) begin
            last_sof_delay   <= 32'd0;
            last_frame_idx   <= 8'd0;
            last_frame_fsin  <= 32'd0;
            last_frame_early <= 1'b0;
        end else if (sof_beat) begin
            last_sof_delay   <= since_fsin;
            last_frame_idx   <= frame_idx;
            last_frame_fsin  <= fsin_count;
            last_frame_early <= early;
        end
    end

    // ---- whole-frame drop gate ------------------------------------------
    // Decision is made on the SOF beat and held until the next SOF.
    reg  pass_r;
    wire pass_now = (s_axis_tvalid & s_axis_tuser) ? ~(drop_en & early) : pass_r;

    always @(posedge aclk) begin
        if (!aresetn) begin
            pass_r        <= 1'b1;
            dropped_count <= 32'd0;
        end else begin
            if (sof_beat) begin
                pass_r <= pass_now;
                if (!pass_now) dropped_count <= dropped_count + 1;
            end
            if (clr_cnt) dropped_count <= 32'd0;
        end
    end

    assign m_axis_tdata  = s_axis_tdata;
    assign m_axis_tuser  = s_axis_tuser;
    assign m_axis_tlast  = s_axis_tlast;
    assign m_axis_tvalid = s_axis_tvalid & pass_now;
    assign s_axis_tready = pass_now ? m_axis_tready : 1'b1;  // sink dropped beats

    // ---- AXI4-Lite slave --------------------------------------------------
    // One transaction at a time: a write is taken when AW and W are both valid,
    // a read when AR is valid; ready pulses for one cycle, then the response is held
    // until accepted. All registers are single-cycle, so responses are OKAY.
    assign s_axi_bresp = 2'b00;
    assign s_axi_rresp = 2'b00;

    wire wr_go = s_axi_awvalid & s_axi_wvalid & ~s_axi_awready & ~s_axi_bvalid;
    wire rd_go = s_axi_arvalid & ~s_axi_arready & ~s_axi_rvalid;

    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
            drop_en       <= 1'b0;
            drop_window   <= 32'd0;
            clr_cnt       <= 1'b0;
        end else begin
            s_axi_awready <= wr_go;
            s_axi_wready  <= wr_go;
            clr_cnt       <= 1'b0;
            if (wr_go) begin
                case (s_axi_awaddr[4:2])
                    3'd0: if (s_axi_wstrb[0]) begin
                              drop_en <= s_axi_wdata[0];
                              clr_cnt <= s_axi_wdata[1];
                          end
                    3'd1: begin
                              if (s_axi_wstrb[0]) drop_window[7:0]   <= s_axi_wdata[7:0];
                              if (s_axi_wstrb[1]) drop_window[15:8]  <= s_axi_wdata[15:8];
                              if (s_axi_wstrb[2]) drop_window[23:16] <= s_axi_wdata[23:16];
                              if (s_axi_wstrb[3]) drop_window[31:24] <= s_axi_wdata[31:24];
                          end
                    default: ;
                endcase
                s_axi_bvalid <= 1'b1;
            end else if (s_axi_bvalid && s_axi_bready) begin
                s_axi_bvalid <= 1'b0;
            end
        end
    end

    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rdata   <= 32'd0;
        end else begin
            s_axi_arready <= rd_go;
            if (rd_go) begin
                case (s_axi_araddr[4:2])
                    3'd0: s_axi_rdata <= {31'd0, drop_en};
                    3'd1: s_axi_rdata <= drop_window;
                    3'd2: s_axi_rdata <= fsin_count;
                    3'd3: s_axi_rdata <= dropped_count;
                    3'd4: s_axi_rdata <= last_sof_delay;
                    3'd5: s_axi_rdata <= {23'd0, last_frame_early, last_frame_idx};
                    3'd6: s_axi_rdata <= last_frame_fsin;
                    3'd7: s_axi_rdata <= 32'h4654_4101;
                endcase
                s_axi_rvalid <= 1'b1;
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

endmodule
