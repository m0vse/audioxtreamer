----------------------------------------------------------------------------------
-- Company: 
-- Engineer: 
-- 
-- Create Date: 07/12/2018 01:13:41 AM
-- Design Name: 
-- Module Name: FIFO2IIC - Behavioral
-- Project Name: 
-- Target Devices: 
-- Tool Versions: 
-- Description: 
-- 
-- Dependencies: 
-- 
-- Revision:
-- Revision 0.01 - File Created
-- Additional Comments:
-- 
----------------------------------------------------------------------------------

library IEEE;
use IEEE.STD_LOGIC_1164.ALL;

-- Uncomment the following library declaration if using
-- arithmetic functions with Signed or Unsigned values
use IEEE.NUMERIC_STD.ALL;
use ieee.std_logic_unsigned.all;
use ieee.std_logic_arith.all;

-- Uncomment the following library declaration if instantiating
-- any Xilinx leaf cells in this code.
library UNISIM;
use UNISIM.VComponents.all;

use work.common_types.all;
------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------
entity CY16_to_IIS is
generic(
sdi_lines   : positive := 12;
sdo_lines   : positive := 12
);
  Port (
    areset : in std_logic;
    --Cypress interface
    cy_clk : in STD_LOGIC; -- 48Mhz
    -- DATA IO
    DIO : inout STD_LOGIC_VECTOR(15 downto 0);
    -- FIFO control
    SLWRn, SLRDn, SLOEn : out STD_LOGIC;
    FIFOADDR0, FIFOADDR1, PKTEND : out STD_LOGIC;
    FLAGA, FLAGB : in STD_LOGIC;
    --LSI ifc
    lsi_clk  : in std_logic;
    lsi_mosi : in std_logic;
    lsi_miso : out std_logic;
    lsi_stop : in std_logic;

    gpio_dat : in std_logic;
    --yamaha external clock and word clock
    ain: in std_logic_vector( 11 downto 0);
    aout: out std_logic_vector( 11 downto 0);

    txb_oe: out std_logic;

    spdif_out1 : out std_logic;
    ymh_clk : in std_logic;
    ymh_word_clk : in std_logic;

    midi_rx : in std_logic_vector( 5 downto 1 ) ;
    midi_tx : out std_logic_vector( 5 downto 1 ) ;

    led: out std_logic_vector (7 downto 0)

    );
end CY16_to_IIS;
------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------

architecture Behavioral of CY16_to_IIS is

constant max_nr_outputs : natural := sdo_lines*2;
constant max_nr_inputs : natural := sdi_lines*2;

function to_std_logic_vector( str : string )
  return std_logic_vector
  is
  variable vec : std_logic_vector( str'length * 8 - 1 downto 0) ;
begin
  for i in 1 to str'high loop
    vec(i * 8 - 1 downto (i - 1) * 8) := std_logic_vector( to_unsigned( character'pos(str(i)) , 8 ) ) ;
  end loop ;
  return vec ;
end function ;

component ezusb_lsi
port (
  clk : in std_logic;
  reset_in: in std_logic;
  reset : out std_logic;
  
  data_clk: in std_logic;-- data sent on both edges, LSB transmitted first
  mosi : in std_logic;
  miso : out std_logic;
  stop : in std_logic;
  -- interface
  in_addr: out std_logic_vector (7 downto 0);-- input address
  in_data: out std_logic_vector (31 downto 0); -- input data
  in_strobe: out std_logic;-- 1 indicates new data received (1 for one cycle)
  in_valid:  out std_logic;-- 1 if date is valid
  out_addr: out std_logic_vector (7 downto 0);-- output address
  out_data: in std_logic_vector (31 downto 0); --output data
  out_strobe: out std_logic -- 1 indicates new data request (1 for one cycle)
);
end component;

signal f256_clk	:std_logic;
signal word_clk	:std_logic;

signal sd_reset : std_logic;
signal usb_clk	:std_logic;
signal usb_reset : std_logic;
signal io_reset : std_logic;
signal clkgen_locked : std_logic;

signal rx_data : std_logic_vector(15 downto 0);

signal usb_rx_empty : std_logic;
signal usb_rdn : std_logic;
signal usb_oe : std_logic;

subtype out_data_array is slv24_array(0 to max_nr_outputs-1);

signal out_fifo_full : std_logic_vector((max_nr_outputs/3)-1 downto 0);
signal out_prog_full : std_logic_vector((max_nr_outputs/3)-1 downto 0);

signal rcvr_wr: std_logic;
signal rcvr_data : out_data_array;
signal rcvr_fifo_full : std_logic;

signal tx_reset : std_logic;

signal sd_in : std_logic_vector(sdi_lines-1 downto 0);
signal sd_out : std_logic_vector(sdo_lines-1 downto 0);

type rd_data_count_t is array (natural range <> ) of std_logic_vector(7 downto 0);
signal rd_data_count : rd_data_count_t( 0 to max_nr_outputs-1);

signal out_fifo_rd_data : out_data_array;
signal out_fifo_rd_en : std_logic;
signal out_fifo_rd : std_logic;
signal out_fifo_empty : std_logic_vector((max_nr_outputs/3)-1 downto 0);
signal out_empty  : std_logic;
signal out_fifo_min : std_logic_vector(7 downto 0);
signal out_fifo_skip : std_logic;
signal out_fifo_skip_count : slv_16;
signal out_fifo_stats_reset : std_logic;

signal xmtr_addr : natural;
signal xmtr_data : slv_16;

signal in_fifo_full_count : slv_16;
signal in_fifo_wr_full : std_logic_vector((max_nr_inputs/3)-1 downto 0);
signal in_fifo_wr_data: slv24_array(0 to max_nr_inputs-1);
signal in_fifo_wr_en : std_logic;

signal in_fifo_rd_empty : std_logic_vector((max_nr_inputs/3)-1 downto 0);
signal in_fifo_rd_data  : slv24_array(0 to max_nr_inputs-1);
signal in_fifo_rd_en : std_logic;
signal in_fifo_empty : std_logic;

signal in_axis_tvalid : std_logic;
signal in_axis_tready : std_logic;
signal in_axis_tlast  : std_logic;
signal in_axis_tdata  : std_logic_vector(15 downto 0);

signal lsi_rd_data : slv_32;
signal lsi_rd_addr : slv_8;

signal lsi_wr_data : slv_32;
signal lsi_wr_addr : slv_8;
signal lsi_wr   : std_logic;

signal reg_sr_count : slv_16;
signal reg_debug : slv_32;
signal reg_ch_params : slv_32;

signal midi_in : slv8_array( 1 to 7) ;
signal midi_in_valid : std_logic_vector(7 downto 1);
signal midi_in_ready : std_logic_vector(7 downto 1);
signal midi_in_valid_r:std_logic_vector(7 downto 1);

signal midi_out : slv8_array( 1 to 7);
signal midi_out_valid : std_logic_vector(7 downto 1);
signal midi_out_ready : std_logic_vector(7 downto 1);
signal midi_7_6_tx : std_logic_vector(1 downto 0); --null
signal midi_out_valid_8: std_logic; --null
signal midi_out_8: slv_8;--null


constant cookie_str :string := "TRTG";
constant cookie : slv_32 := to_std_logic_vector(cookie_str);

attribute mark_debug : string;
attribute keep : string;

attribute mark_debug of usb_oe	: signal is "true";
attribute mark_debug of usb_rdn	: signal is "true";
attribute mark_debug of FLAGA	: signal is "true";
attribute mark_debug of FLAGB	: signal is "true";
attribute mark_debug of out_fifo_skip  : signal is "true";


------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------
begin
txb_oe <= '1';
sd_in <= ain;
aout <= sd_out(11 downto 0);

ymh_bufg : BUFG port map( I => ymh_clk, O => f256_clk );
cy_bufg :  BUFG port map( I => cy_clk, O => usb_clk );

------------------------------------------------------------------------------------------------------------

--clock_gen : entity work.clockgen
--port map
--(
--  clk_48m => usb_clk,
--  clk_12m288 => f256_clk,
--  reset => usb_reset,
--  locked => clkgen_locked,
--  word_clk => word_clk
--);
word_clk <= ymh_word_clk;
clkgen_locked <= areset;
------------------------------------------------------------------------------------------------------------

ezusb_lsi_i : ezusb_lsi
port map
(
  clk => usb_clk,
  reset_in => usb_reset,
  --LSI ifc
  data_clk => lsi_clk,
  miso => lsi_miso,
  mosi => lsi_mosi,
  stop => lsi_stop,
  --registers
  out_data => lsi_rd_data,
  out_addr => lsi_rd_addr,

  in_strobe => lsi_wr,
  in_data => lsi_wr_data,
  in_addr => lsi_wr_addr
);

------------------------------------------------------------------------------------------------------------

with lsi_rd_addr select lsi_rd_data <=
  cookie when X"00", -- the cookie
  X"00000001" when X"01", -- the current version of the fpga
  x"0000" & reg_sr_count when X"02", -- sampling rate counter to detect the word clock
  reg_debug    when X"03",
  reg_ch_params when X"04",

  X"CACABACA" when others;

------------------------------------------------------------------------------------------------------------

proc_reg_debug : process(usb_clk)
begin
  if rising_edge(usb_clk) then
    reg_debug <= ext( gpio_dat & FLAGB  &FLAGA & sd_reset & io_reset & clkgen_locked, reg_debug'length );
  end if;
end process;

------------------------------------------------------------------------------------------------------------
sr_detect : entity work.SamplingRateDetect
  port map
(
  clk => usb_clk,
  rst => usb_reset,
  pcm_clk => f256_clk,
  sr_count => reg_sr_count
);

------------------------------------------------------------------------------------------------------------
bargraph: for i in 1 to led'length-1 generate
led(i) <= '1' when rd_data_count(0)(rd_data_count(0)'length-1 downto rd_data_count(0)'length-3) < i else '0';
end generate;
led(0) <= out_empty;

------------------------------------------------------------------------------------------------------------
--synchronize the ft_reset to the ft_clk
process (usb_clk, areset )
variable ff: std_logic_vector(2 downto 0);
begin
  if areset = '1' then
      ff := (others => '1');
  elsif rising_edge(usb_clk) then
      ff := ff(ff'length-2 downto 0) & '0'; 
  end if;
  usb_reset <= ff(ff'length-1);
end process;
------------------------------------------------------------------------------------------------------------
--synchronize the sd_reset to the sd_f256_clk
process (f256_clk, areset )
variable ff: std_logic_vector(2 downto 0);
begin
  if areset = '1' then
      ff := (others => '1');
  elsif rising_edge(f256_clk) then
      ff := ff(ff'length-2 downto 0) & '0'; 
  end if;
  sd_reset <= ff(ff'length-1);
end process;

------------------------------------------------------------------------------------------------------------
proc_settings : process(usb_clk)
begin
  if rising_edge(usb_clk) then
    if usb_reset = '1' then
      reg_ch_params <= (others => '0');
    elsif lsi_wr = '1' then
      if lsi_wr_addr = X"04" then
        reg_ch_params <= lsi_wr_data;
      end if;
    end if;
  end if;
end process;

------------------------------------------------------------------------------------------------------------

rcvr_fifo_full <= '1' when (out_fifo_full or out_prog_full) /= 0 else '0';

------------------------------------------------------------------------------------------------------------

proc_out_fifo_min : process (usb_clk)
begin
  if rising_edge(usb_clk) then
    if usb_reset = '1' or out_fifo_stats_reset = '1' then
      out_fifo_min <= (others => '1');
	 elsif out_empty = '1' then
	   out_fifo_min <= (others => '0');
   elsif rd_data_count(0) < out_fifo_min then
	   out_fifo_min <= rd_data_count(0);
	 end if;  
  end if;
end process;


proc_out_fifo_skip : process (f256_clk)
begin
  if rising_edge(f256_clk) then
    if sd_reset = '1' or tx_reset = '1' then
      out_fifo_skip_count <= (others => '0');
      out_fifo_skip <= '0';
    elsif rd_data_count(0) = 0 and out_fifo_rd = '1' and out_fifo_skip_count < X"FFFF" then
      out_fifo_skip_count <= out_fifo_skip_count +1;
      out_fifo_skip <= '1';
   else
      out_fifo_skip <= '0';
    end if;
  end if;
end process;
------------------------------------------------------------------------------------------------------------
proc_in_fifo_full : process (f256_clk)
begin
  if rising_edge(f256_clk) then
    if sd_reset = '1' or tx_reset = '1' then
      in_fifo_full_count <= (others => '0');
    elsif in_fifo_wr_full /= 0 and in_fifo_wr_en = '1' and in_fifo_full_count < X"FFFF" then
	   in_fifo_full_count <= in_fifo_full_count +1;
	 end if;
  end if;
end process;

------------------------------------------------------------------------------------------------------------
rcvr : entity work.cy16_to_fifo
generic map(
  max_sdo_lines => sdo_lines
)
port map (
  usb_clk  => usb_clk,
  reset  => io_reset,
  usb_oe   => usb_oe,
  usb_data => rx_data,
  usb_empty => usb_rx_empty,
  usb_rd_req  => usb_rdn,

  nr_outputs  => reg_ch_params(3 downto 0),
  out_fifo_full => rcvr_fifo_full,
  out_fifo_wr   => rcvr_wr,
  out_fifo_data => rcvr_data,

  midi_out_wr(6 downto 0) => midi_out_valid,
  midi_out_wr(7) => midi_out_valid_8,
  midi_out_data(0 to 6) => midi_out,
  midi_out_data(7) => midi_out_8
);
------------------------------------------------------------------------------------------------------------
io_reset <= '1' when  usb_reset = '1' or (lsi_wr = '1' and lsi_wr_addr = X"04") else '0';
------------------------------------------------------------------------------------------------------------

out_empty <= out_fifo_empty(0);
out_fifo_rd <= out_fifo_rd_en;
------------------------------------------------------------------------------------------------------------

g_out_fifos : for i in 0 to (max_nr_outputs/3) -1 generate

  output_fifo_i: entity work.ASYNCH_FIFO_72x256
    port map (
      empty      => out_fifo_empty(i),

      dout(23 downto  0)  => out_fifo_rd_data(i*3),
      dout(47 downto 24)  => out_fifo_rd_data(i*3+1),
      dout(71 downto 48)  => out_fifo_rd_data(i*3+2),

      rd_en      => out_fifo_rd,

      full      => out_fifo_full(i),

      din(23 downto  0)  => rcvr_data(i*3),
      din(47 downto 24)  => rcvr_data(i*3+1),
      din(71 downto 48)  => rcvr_data(i*3+2),

      wr_en      => rcvr_wr,
      rd_data_count  => rd_data_count(i),

      prog_full  => out_prog_full(i),
      prog_full_thresh => reg_ch_params(23 downto 16),
      --prog_full_thresh(8 downto 1) => reg_ch_params(23 downto 16),
      --prog_full_thresh(0) => '0',
      rd_rst  => sd_reset,
      rd_clk  => f256_clk,
      wr_rst  => io_reset,
      wr_clk  => usb_clk
    );  
end generate;
------------------------------------------------------------------------------------------------------------
pcmio_inst: entity work.pcmio
  generic map (
    sdo_lines        => sdo_lines,
    sdi_lines        => sdi_lines
  )
  port map (
    f256_clk         => f256_clk,
    sd_reset         => sd_reset,
    word_clk         => word_clk,
    spdif_out1       => spdif_out1,
    out_fifo_rd_data => out_fifo_rd_data,
    out_empty        => out_empty,
    out_fifo_rd_en   => out_fifo_rd_en,
    in_fifo_wr_data  => in_fifo_wr_data,
    in_fifo_wr_en    => in_fifo_wr_en,
    sd_in            => sd_in,
    sd_out           => sd_out
  );

------------------------------------------------------------------------------------------------------------
in_fifos : for i in 0 to (max_nr_inputs/3)-1 generate
  
    input_fifo_i : entity work.ASYNCH_FIFO_72x256
     port map (
      empty 	=> in_fifo_rd_empty(i),

      dout(23 downto  0)  => in_fifo_rd_data(i*3),
      dout(47 downto 24)  => in_fifo_rd_data(i*3+1),
      dout(71 downto 48)  => in_fifo_rd_data(i*3+2),

      rd_en   => in_fifo_rd_en,
      full    => in_fifo_wr_full(i),
      prog_full_thresh => (others => '1'),
      din(23 downto  0)  => in_fifo_wr_data(i*3),
      din(47 downto 24)  => in_fifo_wr_data(i*3+1),
      din(71 downto 48)  => in_fifo_wr_data(i*3+2),

      wr_en 	=> in_fifo_wr_en,
      rd_rst	=> io_reset,
      rd_clk	=> usb_clk,
      wr_rst	=> sd_reset,
      wr_clk	=> f256_clk
    );
end generate;
------------------------------------------------------------------------------------------------------------

in_fifo_empty <= '0' when in_fifo_rd_empty = 0 else '1';

------------------------------------------------------------------------------------------------------------

with xmtr_addr select xmtr_data <=
  reg_sr_count                when 2,
  X"00" & rd_data_count(0)    when 3,
  out_fifo_skip_count         when 4,
  in_fifo_full_count          when 5,
  midi_in(1) & '0' & midi_in_valid_r when 6,
  midi_in(3) & midi_in(2)     when 7,
  midi_in(5) & midi_in(4)     when 8,
  midi_in(7) & midi_in(6)     when 9,
  X"CDCD" when others;


--latch the channels when reading @ 0005 to avoid sending data arrived after that
p_midi_in_valid : process(usb_clk)
begin
  if rising_edge(usb_clk) then
    if xmtr_addr = 5 then
      midi_in_valid_r <= midi_in_valid;
    end if;
  end if;
end process;


p_midi_in_ready : process(xmtr_addr,midi_in_valid_r)

begin
  midi_in_ready <= (others=>'0');
  case xmtr_addr is
    when 6 =>
      midi_in_ready(1) <= midi_in_valid_r(1);
    when 7 =>
      midi_in_ready(2) <= midi_in_valid_r(2);
      midi_in_ready(3) <= midi_in_valid_r(3);
    when 8 =>
      midi_in_ready(4) <= midi_in_valid_r(4);
      midi_in_ready(5) <= midi_in_valid_r(5);
    when 9 =>
      midi_in_ready(6) <= midi_in_valid_r(6);
      midi_in_ready(7) <= midi_in_valid_r(7);
    when others =>
      null;
  end case;
end process;

------------------------------------------------------------------------------------------------------------
tx_reset <= '1' when reg_ch_params = 0 else '0';
------------------------------------------------------------------------------------------------------------
xmtr: entity work.fifo_to_m_axis
generic map (
  max_sdi_lines => sdi_lines
) port map (
  --cypress interface
  clk  => usb_clk,
  reset=> tx_reset,

  -- status report
  data_in         => xmtr_data,
  data_addr       => xmtr_addr,

  nr_inputs       => reg_ch_params(7 downto 4),
  sof_int         => gpio_dat,

  in_fifo_empty   => in_fifo_empty,
  in_fifo_rd      => in_fifo_rd_en,
  in_fifo_data    => in_fifo_rd_data,

  m_axis_tvalid   => in_axis_tvalid,
  m_axis_tlast    => in_axis_tlast,
  m_axis_tready   => in_axis_tready,
  m_axis_tdata    => in_axis_tdata
);
------------------------------------------------------------------------------------------------------------
i_s_axis_to_w_fifo: entity work.s_axis_to_w_fifo
  generic map (
    DATA_WIDTH => 16
  ) port map (
    clk  => usb_clk,
    reset=> usb_reset,

-- FX2LP ifc
    DIO => DIO,

    FIFOADDR0 => FIFOADDR0,
    FIFOADDR1 => FIFOADDR1,
    FLAGA => FLAGA,
    FLAGB => FLAGB,

    SLRDn => SLRDn,
    SLWRn => SLWRn,
    PKTEND => PKTEND,
    SLOEn => SLOEn,

--s_axis m_axis
    m_axis_tready => '0',
    rx_data  => rx_data,
    rx_empty => usb_rx_empty,
    rx_rdn => usb_rdn,
    rx_oe  => usb_oe,

    s_axis_tvalid => in_axis_tvalid,
    s_axis_tlast  => in_axis_tlast,
    s_axis_tready => in_axis_tready,
    s_axis_tdata  => in_axis_tdata
  );
------------------------------------------------------------------------------------------------------------
i_midi_io : entity work.midi_io
generic map ( NR_CHANNELS => 7 )
port map
(
  clk             => usb_clk,
  reset           => usb_reset,

  rx(5 downto 1)  => midi_rx,
  rx(7 downto 6)  => "00",
  rx_data         => midi_in,
  rx_valid        => midi_in_valid,
  rx_ready        => midi_in_ready,
  rx_fifo_reset   => tx_reset,

  tx(5 downto 1)  => midi_tx,
  tx(7 downto 6)  => midi_7_6_tx,
  tx_data         => midi_out,
  tx_ready        => midi_out_ready,
  tx_valid        => midi_out_valid
);

------------------------------------------------------------------------------------------------------------
end Behavioral;
