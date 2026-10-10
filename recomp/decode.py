"""Decode raw words into R5900 instructions with rabbitizer."""
import rabbitizer as rz

# Codegen needs the real instructions, not assembler pseudo-ops like `b`, `move` or `beqz`.
rz.config.pseudos_enablePseudos = False
rz.config.toolchainTweaks_treatJAsUnconditionalBranch = False


def decode(word: int, vram: int) -> rz.Instruction:
    return rz.Instruction(word, vram=vram, category=rz.InstrCategory.R5900)


def decode_range(data: bytes, vram: int) -> list[rz.Instruction]:
    return [decode(int.from_bytes(data[i:i + 4], "little"), vram + i)
            for i in range(0, len(data) - 3, 4)]
