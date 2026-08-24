.section .data

.word 0x80371240       /* PI BSB Domain 1 register */
.word 0x0000000F       /* Clockrate setting */
.word 0x80070C00       /* Entrypoint address */
.word 0x0000144A       /* Revision */
.word 0x0ADAECA7       /* Checksum 1 */
.word 0xB17F9795       /* Checksum 2 */
.word 0x00000000       /* Unknown 1 */
.word 0x00000000       /* Unknown 2 */
.ascii "OgreBattle64        " /* Internal name */
.word 0x00000000       /* Unknown 3 */
.word 0x0000004E       /* Cartridge */
.ascii "OB"            /* Cartridge ID */
.ascii "E"             /* Country code */
.byte 0x01             /* Version */
