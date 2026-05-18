# Custom PCB Pin Mapping Reference Manual (ATmega4809)

This reference manual documents the physical header pinouts and underlying microcontroller register mappings for the custom CD-ROM emulation board based on the **ATmega4809** (megaAVR 0-series). 


Left Vertical Header (Main Signaling)
    • LD–ON $\rightarrow$ D8 / Port E, Pin 3 (PE3)
    • SQSO $\rightarrow$ D13 / Port E, Pin 2 (PE2) (SPI SCK)
    • SQCK $\rightarrow$ D12 / Port E, Pin 1 (PE1) (SPI CIPO / SC1)
    • MUTE $\rightarrow$ D11 / Port E, Pin 0 (PE0) (SPI COPI / SC1)
    • SENS $\rightarrow$ AREF / Port D, Pin 7 (PD7)
    • DATA $\rightarrow$ A7 / D21 / Port D, Pin 5 (PD5) (AIN[5])
    • XLAT $\rightarrow$ A6 / D20 / Port D, Pin 4 (PD4) (AIN[4])
    • CLOK $\rightarrow$ A0 / D14 / Port D, Pin 3 (PD3) (AIN[3])
    • FOK $\rightarrow$ A1 / D15 / Port D, Pin 2 (PD2) (AIN[2])
    • GFS $\rightarrow$ A2 / D16 / Port D, Pin 1 (PD1) (AIN[1])
Right Vertical Header (Interface & Control)
    • DOOR $\rightarrow$ ~D9 / Port B, Pin 0 (PB0)
    • ACTIVE $\rightarrow$ ~D10 / Port B, Pin 1 (PB1)
    • PASSIVE $\rightarrow$ ~D5 / Port B, Pin 2 (PB2)
    • IF_DIR (XDIR) $\rightarrow$ A5 / D19 / Port A, Pin 3 / Port F, Pin 3 (PA3/PF3) (SCL)
    • IF_CLK (XCLK) $\rightarrow$ A4 / D18 / Port A, Pin 2 / Port F, Pin 2 (PA2/PF2) (SDA)
    • IF_DATA (TXD) $\rightarrow$ D2 / Port A, Pin 0 (PA0)
    • SUB_SCOR $\rightarrow$ A3 / D17 / Port D, Pin 0 (PD0) (AIN[0])
    • RESET / UPDI $\rightarrow$ Dedicated hardware RESET pin (Pin 41 on the physical IC package)
6-Pin SPI/ICSP Header
    • MISO $\rightarrow$ D12 / Port E, Pin 1 (PE1)
    • MOSI $\rightarrow$ D11 / Port E, Pin 0 (PE0)
    • SCK $\rightarrow$ D13 / Port E, Pin 2 (PE2)
    • RESET $\rightarrow$ Hardware RESET / UPDI data line
    • +5V $\rightarrow$ VCC Power Rail
    • GND $\rightarrow$ Ground Plane
Hardware Serial (Bottom Right Isolation Pins)
    • RX $\rightarrow$ Port C, Pin 5 (PC5)
    • TX $\rightarrow$ Port C, Pin 4 (PC4)
