FreeCUPL examples

Each design has a simulation input (.si) with the expected outputs.

  hex7seg   g16v8    hex to seven-segment decoder; fields, TABLE,
                     blanking input
  counter4  g22v10   4-bit up/down counter; .D registers, .AR and .SP,
                     $DEFINE, terminal count
  traffic   g16v8    traffic-light controller; SEQUENCE state machine
                     with conditional and default transitions, OUT
  addrdec   p16l8    memory and I/O address decoder; active-low pins,
                     range match field:[lo..hi]
  shift8    g22v10   8-bit shift register; $REPEAT with {i-1}, APPEND,
                     three-state output with .OE
  div5      v750c    divide-by-5 counter in buried registers; PINNODE,
                     product-term clock .CK, .AR

To compile and simulate one:

    freecupl -jas counter4.pld

This writes counter4.jed with the test vectors appended, counter4.abs,
the listing counter4.so and the waveform file counter4.wo.  A vector
that does not match is flagged in the listing and the compiler prints
"Simulation errors encountered".  -x adds a documentation file (.doc),
-f a fuse plot in it.

To run the simulator on its own:

    freecupl -a counter4.pld
    freecsim -l counter4
