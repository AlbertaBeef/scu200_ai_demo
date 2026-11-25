// fsin_frame_tag.v
//
// Sits on the AXI4-Stream video path in front of frmbuf_wr.
//  * Times every frame's start-of-frame (tuser) relative to the last FSIN.
//  * Latches that delay, an FSIN counter and a frame-in-strobe index into
//    registers that the frmbuf_wr ISR can read (connect to AXI GPIO or your
//    own AXI-Lite register block).
//  * Optional (drop_en = 1): discards any frame whose SOF arrives less than
//    DROP_WINDOW cycles after FSIN, so frmbuf_wr only ever sees the good
//    frame and no dummy buffer is needed.
//
// Classification is by time since FSIN, not by counting frames, so a
// missing first frame doesn't cause the good frame to be dropped.
//
// From the fcapz trace: first frame SOF ~1.06 ms after FSIN, second ~26.05 ms.
// A window of ~13 ms sits between them. Example: 150 MHz -> 1_950_000 cycles.
// scu200_ai_demo03 runs it on the 300 MHz video clock: DROP_WINDOW = 3_900_000.
// (IPI interface attributes on aclk/aresetn added for the BD module reference.)

module fsin_frame_tag #(
    parameter integer TDATA_W     = 24,
    parameter integer DROP_WINDOW = 1_950_000   // cycles of aclk
)(
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axis:m_axis, ASSOCIATED_RESET aresetn" *)
    input  wire                 aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire                 aresetn,
    input  wire                 fsin_async,     // FSIN strobe, active high
    input  wire                 drop_en,        // 1 = drop early frames

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

    // status, read by software
    output reg  [31:0]          fsin_count,       // FSIN edges seen
    output reg  [31:0]          last_sof_delay,   // cycles FSIN->SOF, latest frame
    output reg  [7:0]           last_frame_idx,   // 0 = first frame after FSIN
    output reg  [31:0]          last_frame_fsin,  // fsin_count at that SOF
    output reg                  last_frame_early, // 1 = classified as dummy
    output reg  [31:0]          dropped_count
);

    // ---- FSIN synchroniser + rising-edge detect --------------------------
    reg [2:0] fsin_sr;
    always @(posedge aclk) fsin_sr <= {fsin_sr[1:0], fsin_async};
    wire fsin_rise = fsin_sr[1] & ~fsin_sr[2];

    // ---- time since FSIN (saturating) and frame index --------------------
    reg [31:0] since_fsin;
    reg [7:0]  frame_idx;

    wire sof_beat = s_axis_tvalid & s_axis_tuser & s_axis_tready;
    wire early    = (since_fsin < DROP_WINDOW);

    always @(posedge aclk) begin
        if (!aresetn) begin
            since_fsin <= 32'hFFFF_FFFF;   // "long ago" until first FSIN
            frame_idx  <= 8'd0;
            fsin_count <= 32'd0;
        end else if (fsin_rise) begin
            since_fsin <= 32'd0;
            frame_idx  <= 8'd0;
            fsin_count <= fsin_count + 1;
        end else begin
            if (since_fsin != 32'hFFFF_FFFF) since_fsin <= since_fsin + 1;
            if (sof_beat && frame_idx != 8'hFF) frame_idx <= frame_idx + 1;
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
        end else if (sof_beat) begin
            pass_r <= pass_now;
            if (!pass_now) dropped_count <= dropped_count + 1;
        end
    end

    assign m_axis_tdata  = s_axis_tdata;
    assign m_axis_tuser  = s_axis_tuser;
    assign m_axis_tlast  = s_axis_tlast;
    assign m_axis_tvalid = s_axis_tvalid & pass_now;
    assign s_axis_tready = pass_now ? m_axis_tready : 1'b1;  // sink dropped beats

endmodule
