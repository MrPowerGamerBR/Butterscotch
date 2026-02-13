package com.mrpowergamerbr.butterscotch.vm

import com.mrpowergamerbr.butterscotch.Butterscotch
import com.mrpowergamerbr.butterscotch.data.GameData
import com.mrpowergamerbr.butterscotch.runtime.GameRunner
import com.mrpowergamerbr.butterscotch.runtime.Instance

class DecodedCode(
    val instructions: List<Instruction>,
    val offsetToIndex: Map<Int, Int>,
)

class VM(val gameData: GameData) {
    lateinit var runner: GameRunner

    // Pre-decoded instructions per code entry
    val decodedEntries = arrayOfNulls<DecodedCode>(gameData.codeEntries.size)

    // Built-in function registry: name -> implementation
    val builtinFunctions = mutableMapOf<String, (VM, List<GMLValue>) -> GMLValue>()

    // Function name -> index in gameData.functions
    val functionNameToIndex = mutableMapOf<String, Int>()

    // Script name -> code entry index
    val scriptNameToCodeId = mutableMapOf<String, Int>()

    // Variable name -> VARI entry index (for lookup during execution)
    val variableNameToIndices = mutableMapOf<String, MutableList<Int>>()

    fun initialize() {
        // Build function name lookup
        for ((i, func) in gameData.functions.withIndex()) {
            functionNameToIndex[func.name] = i
        }

        // Build script lookup
        for (script in gameData.scripts) {
            scriptNameToCodeId[script.name] = script.codeId
        }

        // Build variable name lookup
        for ((i, v) in gameData.variables.withIndex()) {
            variableNameToIndices.getOrPut(v.name) { mutableListOf() }.add(i)
        }

        // Decode all code entries and resolve references
        for (i in gameData.codeEntries.indices) {
            val entry = gameData.codeEntries[i]
            if (entry.bytecode.isEmpty()) {
                decodedEntries[i] = DecodedCode(emptyList(), emptyMap())
                continue
            }
            val (instructions, offsetMap) = decodeBytecode(entry.bytecode)
            decodedEntries[i] = DecodedCode(instructions, offsetMap)
        }

        // Resolve VARI reference chains
        resolveVariableChains()

        // Resolve FUNC reference chains
        resolveFunctionChains()

        println("VM initialized: ${decodedEntries.size} code entries decoded")
    }

    private fun resolveVariableChains() {
        // Build sorted list of (bytecodeAbsoluteOffset, codeEntryIndex) for binary search
        data class CodeRange(val start: Int, val end: Int, val index: Int)
        val ranges = gameData.codeEntries.mapIndexedNotNull { i, e ->
            if (e.bytecodeLength > 0) CodeRange(e.bytecodeAbsoluteOffset, e.bytecodeAbsoluteOffset + e.bytecodeLength, i) else null
        }.sortedBy { it.start }

        fun findCodeEntry(addr: Int): CodeRange? {
            var lo = 0; var hi = ranges.size - 1
            while (lo <= hi) {
                val mid = (lo + hi) / 2
                val r = ranges[mid]
                when {
                    addr < r.start -> hi = mid - 1
                    addr >= r.end -> lo = mid + 1
                    else -> return r
                }
            }
            return null
        }

        var resolved = 0
        for ((varIdx, v) in gameData.variables.withIndex()) {
            if (v.occurrenceCount <= 0 || v.firstOccurrenceOffset < 0) continue

            var instrAddr = v.firstOccurrenceOffset
            for (i in 0 until v.occurrenceCount) {
                val range = findCodeEntry(instrAddr)
                if (range == null) {
                    // Try to continue chain anyway
                    break
                }

                val localOffset = instrAddr - range.start
                val decoded = decodedEntries[range.index] ?: break
                val instrIndex = decoded.offsetToIndex[localOffset]
                if (instrIndex == null) {
                    break
                }

                val instr = decoded.instructions[instrIndex]
                instr.variableIndex = varIdx
                resolved++

                // Read chain link from raw bytecode to advance
                if (i < v.occurrenceCount - 1) {
                    val entry = gameData.codeEntries[range.index]
                    val refOffset = localOffset + 4
                    if (refOffset + 4 <= entry.bytecode.size) {
                        val raw = readI32(entry.bytecode, refOffset)
                        val nextOffset = raw and 0x07FFFFFF
                        instrAddr += nextOffset
                    } else break
                }
            }
        }
        println("  Resolved $resolved variable references")
    }

    private fun resolveFunctionChains() {
        data class CodeRange(val start: Int, val end: Int, val index: Int)
        val ranges = gameData.codeEntries.mapIndexedNotNull { i, e ->
            if (e.bytecodeLength > 0) CodeRange(e.bytecodeAbsoluteOffset, e.bytecodeAbsoluteOffset + e.bytecodeLength, i) else null
        }.sortedBy { it.start }

        fun findCodeEntry(addr: Int): CodeRange? {
            var lo = 0; var hi = ranges.size - 1
            while (lo <= hi) {
                val mid = (lo + hi) / 2
                val r = ranges[mid]
                when {
                    addr < r.start -> hi = mid - 1
                    addr >= r.end -> lo = mid + 1
                    else -> return r
                }
            }
            return null
        }

        var resolved = 0
        for ((funcIdx, f) in gameData.functions.withIndex()) {
            if (f.occurrenceCount <= 0 || f.firstOccurrenceOffset < 0) continue

            var instrAddr = f.firstOccurrenceOffset
            for (i in 0 until f.occurrenceCount) {
                val range = findCodeEntry(instrAddr)
                if (range == null) break

                val localOffset = instrAddr - range.start
                val decoded = decodedEntries[range.index] ?: break
                val instrIndex = decoded.offsetToIndex[localOffset]
                if (instrIndex == null) break

                val instr = decoded.instructions[instrIndex]
                instr.functionIndex = funcIdx
                resolved++

                if (i < f.occurrenceCount - 1) {
                    val entry = gameData.codeEntries[range.index]
                    val refOffset = localOffset + 4
                    if (refOffset + 4 <= entry.bytecode.size) {
                        val raw = readI32(entry.bytecode, refOffset)
                        val nextOffset = raw and 0x07FFFFFF
                        instrAddr += nextOffset
                    } else break
                }
            }
        }
        println("  Resolved $resolved function references")
    }

    private fun readI32(bytes: ByteArray, offset: Int): Int {
        val b0 = bytes[offset].toInt() and 0xFF
        val b1 = bytes[offset + 1].toInt() and 0xFF
        val b2 = bytes[offset + 2].toInt() and 0xFF
        val b3 = bytes[offset + 3].toInt() and 0xFF
        return b0 or (b1 shl 8) or (b2 shl 16) or (b3 shl 24)
    }

    // ========== Execution ==========

    fun executeCode(codeEntryIndex: Int, self: Instance, other: Instance? = null) {
        if (codeEntryIndex < 0 || codeEntryIndex >= decodedEntries.size) return
        val decoded = decodedEntries[codeEntryIndex] ?: return
        if (decoded.instructions.isEmpty()) return

        val entryName = gameData.codeEntries[codeEntryIndex].name
        // println("  EXEC: $entryName (idx=$codeEntryIndex)")
        val tracing = Butterscotch.traceInstructions.contains("*") || entryName in Butterscotch.traceInstructions
        val prevTrace = traceCodeEntry

        val stack = ArrayDeque<GMLValue>()
        val locals = mutableMapOf<Int, GMLValue>() // varId -> value
        var pc = 0
        val instructions = decoded.instructions
        var currentSelf = self
        var currentOther = other ?: self
        data class EnvIteration(
            val instances: List<Instance>,
            var currentIdx: Int,
            val prevSelf: Instance,
            val prevOther: Instance
        )
        val envIterStack = ArrayDeque<EnvIteration>()

        val maxInstructions = 10_000_000
        var count = 0

        while (pc < instructions.size && count < maxInstructions) {
            count++
            val instr = instructions[pc]
            if (tracing && count < 5000) {
                val varName = if (instr.variableIndex >= 0) gameData.variables[instr.variableIndex].name else ""
                val varInstType = if (instr.variableIndex >= 0) gameData.variables[instr.variableIndex].instanceType else 0
                val funcName = if (instr.functionIndex >= 0) gameData.functions[instr.functionIndex].name else ""
                val stackTop = if (stack.isNotEmpty()) stack.last().toStr() else "empty"
                println("    [$entryName] pc=$pc op=0x${instr.opcode.toString(16)} t1=${instr.type1} t2=${instr.type2} extra=${instr.extra} varInstType=$varInstType instrVarType=0x${instr.variableType.toString(16)} var=$varName func=$funcName stack=${stack.size} top=$stackTop")
            }
            pc++

            try {
                when (instr.opcode) {
                    Opcodes.PUSH -> {
                        val value = when (instr.type1) {
                            DataTypes.DOUBLE -> GMLValue.of(instr.doubleValue)
                            DataTypes.FLOAT -> GMLValue.of(instr.floatValue.toDouble())
                            DataTypes.INT32 -> GMLValue.of(instr.intValue.toDouble())
                            DataTypes.INT64 -> GMLValue.of(instr.longValue.toDouble())
                            DataTypes.BOOLEAN -> GMLValue.of(if (instr.intValue != 0) 1.0 else 0.0)
                            DataTypes.STRING -> {
                                val idx = instr.stringIndex
                                if (idx in gameData.strings.indices) GMLValue.of(gameData.strings[idx])
                                else GMLValue.EMPTY_STRING
                            }
                            DataTypes.INT16 -> GMLValue.of(instr.intValue.toDouble())
                            DataTypes.VARIABLE -> {
                                val varIdx = instr.variableIndex
                                if (varIdx >= 0) {
                                    val v = gameData.variables[varIdx]
                                    val isArray = instr.variableType == 0x00
                                    val isStacktop = instr.variableType == 0x80
                                    // For array PUSH: stack has [instanceTarget, index] with index on top
                                    val arrayIdx = if (isArray) (if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO).toInt() else -1
                                    val arrayInstTarget = if (isArray) (if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO).toInt() else 0
                                    if (isStacktop) {
                                        // Dot-access: instance ID is on the stack (e.g., writer.x)
                                        val targetId = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                                        val target = runner.findInstancesByObjectOrId(targetId.toInt()).firstOrNull()
                                        target?.getBuiltinOrVar(v.name) ?: GMLValue.ZERO
                                    } else {
                                    val effectiveInstType = if (isArray) arrayInstTarget else {
                                        val raw = instr.extra
                                        if (raw != 0) raw else v.instanceType
                                    }
                                    when {
                                        effectiveInstType == InstanceTypes.LOCAL -> {
                                            if (isArray) {
                                                val arr = locals[v.varId]
                                                if (arr is GMLValue.ArrayVal) arr.data[0]?.get(arrayIdx) ?: GMLValue.ZERO
                                                else GMLValue.ZERO
                                            } else {
                                                locals[v.varId] ?: GMLValue.ZERO
                                            }
                                        }
                                        effectiveInstType == InstanceTypes.GLOBAL -> {
                                            if (isArray) getGlobalArrayElement(v.name, arrayIdx)
                                            else runner.globalVariables[v.name] ?: GMLValue.ZERO
                                        }
                                        effectiveInstType == InstanceTypes.STACKTOP -> {
                                            val targetId = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                                            val target = runner.findInstancesByObjectOrId(targetId.toInt()).firstOrNull()
                                            if (isArray) target?.getArrayElement(v.name, arrayIdx) ?: GMLValue.ZERO
                                            else target?.getBuiltinOrVar(v.name) ?: GMLValue.ZERO
                                        }
                                        else -> {
                                            // Check global builtin arrays first (view_wview, etc.)
                                            if (isArray) {
                                                getBuiltinArrayElement(v.name, arrayIdx) ?: run {
                                                    val target = resolveInstance(effectiveInstType, currentSelf, currentOther)
                                                    if (target != null) target.getArrayElement(v.name, arrayIdx)
                                                    else getGlobalArrayElement(v.name, arrayIdx)
                                                }
                                            } else {
                                                val target = resolveInstance(effectiveInstType, currentSelf, currentOther)
                                                if (target != null) target.getBuiltinOrVar(v.name)
                                                else getGlobalBuiltin(v.name)
                                            }
                                        }
                                    }
                                    }
                                } else GMLValue.ZERO
                            }
                            else -> GMLValue.ZERO
                        }
                        stack.addLast(value)
                    }

                    Opcodes.PUSHI -> {
                        stack.addLast(GMLValue.of(instr.intValue.toDouble()))
                    }

                    Opcodes.PUSHLOC -> {
                        val varIdx = instr.variableIndex
                        if (varIdx >= 0) {
                            val v = gameData.variables[varIdx]
                            val isArray = instr.variableType == 0x00
                            val value = if (isArray) {
                                val arrayIdx = (if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO).toInt()
                                val arrayInstTarget = (if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO).toInt()
                                // For locals, just read the array (ignore instance target)
                                val arr = locals[v.varId]
                                if (arr is GMLValue.ArrayVal) arr.data[0]?.get(arrayIdx) ?: GMLValue.ZERO
                                else GMLValue.ZERO
                            } else {
                                locals[v.varId] ?: GMLValue.ZERO
                            }
                            stack.addLast(value)
                        } else {
                            stack.addLast(GMLValue.ZERO)
                        }
                    }

                    Opcodes.PUSHGLB -> {
                        val varIdx = instr.variableIndex
                        if (varIdx >= 0) {
                            val v = gameData.variables[varIdx]
                            val isArray = instr.variableType == 0x00
                            val value = if (isArray) {
                                val arrayIdx = (if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO).toInt()
                                val arrayInstTarget = (if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO).toInt()
                                getGlobalArrayElement(v.name, arrayIdx)
                            } else {
                                runner.globalVariables[v.name] ?: GMLValue.ZERO
                            }
                            stack.addLast(value)
                        } else {
                            stack.addLast(GMLValue.ZERO)
                        }
                    }

                    Opcodes.PUSHBLTN -> {
                        val varIdx = instr.variableIndex
                        if (varIdx >= 0) {
                            val v = gameData.variables[varIdx]
                            val isArray = instr.variableType == 0x00
                            val arrayIdx = if (isArray) (if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO).toInt() else -1
                            val effectiveInstType = if (isArray) {
                                (if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO).toInt()
                            } else {
                                instr.instanceType
                            }
                            // Handle global builtin arrays (view_*, etc.) that aren't per-instance
                            val value = if (isArray) {
                                getBuiltinArrayElement(v.name, arrayIdx) ?: run {
                                    val target = resolveInstance(effectiveInstType, currentSelf, currentOther)
                                    if (target != null) target.getArrayElement(v.name, arrayIdx)
                                    else getGlobalArrayElement(v.name, arrayIdx)
                                }
                            } else {
                                val target = resolveInstance(effectiveInstType, currentSelf, currentOther)
                                if (target != null) target.getBuiltinOrVar(v.name)
                                else getGlobalBuiltin(v.name)
                            }
                            stack.addLast(value)
                        } else {
                            stack.addLast(GMLValue.ZERO)
                        }
                    }

                    Opcodes.POP -> {
                        val varIdx = instr.variableIndex
                        if (varIdx >= 0) {
                            val v = gameData.variables[varIdx]
                            val isArray = instr.variableType == 0x00
                            val isStacktop = instr.variableType == 0x80

                            // For array POP: stack has [value, instanceTarget, index] with index on top
                            // Pop index first, then instance target (magic/-1 for self), then value
                            val arrayIdx = if (isArray) (if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO).toInt() else -1
                            // For array access, GM:S pushes an instance target between value and index
                            val arrayInstTarget = if (isArray) (if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO).toInt() else 0

                            if (isStacktop) {
                                // Dot-access store: stack has [value, instanceTarget] with instanceTarget on top
                                // Pop instanceTarget first, then value
                                val targetId = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                                val value = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                                val target = runner.findInstancesByObjectOrId(targetId.toInt()).firstOrNull()
                                if (target != null) {
                                    traceInstanceVarWrite(target, v.name, value, -1, entryName)
                                    target.setBuiltinOrVar(v.name, value)
                                }
                            } else {
                            val value = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                            // Determine instance type: for array, use the popped instance target;
                            // for non-array, use instr.extra or fall back to VARI instanceType
                            val rawInstType = instr.extra
                            val effectiveInstType = if (isArray) {
                                arrayInstTarget // instance target from stack (e.g., -1 for SELF)
                            } else if (rawInstType != 0) {
                                rawInstType // negative = special type (SELF/OTHER/GLOBAL/etc), positive = object ID
                            } else {
                                v.instanceType
                            }

                            when {
                                effectiveInstType == InstanceTypes.LOCAL -> {
                                    if (isArray) {
                                        val arr = locals.getOrPut(v.varId) { GMLValue.ArrayVal() } as? GMLValue.ArrayVal
                                        if (arr != null) {
                                            arr.data.getOrPut(0) { mutableMapOf() }[arrayIdx] = value
                                        } else {
                                            val newArr = GMLValue.ArrayVal()
                                            newArr.data.getOrPut(0) { mutableMapOf() }[arrayIdx] = value
                                            locals[v.varId] = newArr
                                        }
                                    } else {
                                        locals[v.varId] = value
                                    }
                                }
                                effectiveInstType == InstanceTypes.GLOBAL -> {
                                    if (isArray) setGlobalArrayElement(v.name, arrayIdx, value)
                                    else {
                                        traceGlobalWrite(v.name, value, entryName)
                                        runner.globalVariables[v.name] = value
                                    }
                                }
                                rawInstType == InstanceTypes.STACKTOP -> {
                                    // value (popped above) actually holds the instanceTarget (stack ordering: [realValue, target] with target on top)
                                    val realValue = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                                    val target = runner.findInstancesByObjectOrId(value.toInt()).firstOrNull()
                                    if (target != null) {
                                        traceInstanceVarWrite(target, v.name, realValue, if (isArray) arrayIdx else -1, entryName)
                                        if (isArray) target.setArrayElement(v.name, arrayIdx, realValue)
                                        else target.setBuiltinOrVar(v.name, realValue)
                                    }
                                }
                                else -> {
                                    // Check builtin arrays first (view_xview, etc.)
                                    if (isArray && setBuiltinArrayElement(v.name, arrayIdx, value)) {
                                        // handled
                                    } else if (effectiveInstType >= 0) {
                                        // Object ID: GM sets variable on ALL instances of this object
                                        val targets = runner.findInstancesByObjectOrId(effectiveInstType)
                                        for (target in targets) {
                                            traceInstanceVarWrite(target, v.name, value, if (isArray) arrayIdx else -1, entryName)
                                            if (isArray) target.setArrayElement(v.name, arrayIdx, value)
                                            else target.setBuiltinOrVar(v.name, value)
                                        }
                                    } else {
                                        val target = resolveInstance(effectiveInstType, currentSelf, currentOther)
                                        if (target != null) {
                                            traceInstanceVarWrite(target, v.name, value, if (isArray) arrayIdx else -1, entryName)
                                            if (isArray) target.setArrayElement(v.name, arrayIdx, value)
                                            else target.setBuiltinOrVar(v.name, value)
                                        } else {
                                            traceInstanceVarWrite(currentSelf, v.name, value, if (isArray) arrayIdx else -1, entryName)
                                            if (isArray) currentSelf.setArrayElement(v.name, arrayIdx, value)
                                            else currentSelf.setBuiltinOrVar(v.name, value)
                                        }
                                    }
                                }
                            }
                            }
                        } else {
                            // No variable index - just pop and discard
                            if (stack.isNotEmpty()) stack.removeLast()
                        }
                    }

                    Opcodes.POPZ -> {
                        if (stack.isNotEmpty()) stack.removeLast()
                    }

                    Opcodes.DUP -> {
                        if (stack.isNotEmpty()) {
                            stack.addLast(stack.last())
                        }
                    }

                    Opcodes.ADD -> {
                        val b = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                        val a = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                        val result = if (a is GMLValue.Str || b is GMLValue.Str) {
                            GMLValue.of(a.toStr() + b.toStr())
                        } else {
                            GMLValue.of(a.toReal() + b.toReal())
                        }
                        stack.addLast(result)
                    }
                    Opcodes.SUB -> binaryOp(stack) { a, b -> a - b }
                    Opcodes.MUL -> binaryOp(stack) { a, b -> a * b }
                    Opcodes.DIV -> binaryOp(stack) { a, b -> if (b != 0.0) a / b else 0.0 }
                    Opcodes.REM, Opcodes.MOD -> binaryOp(stack) { a, b -> if (b != 0.0) a % b else 0.0 }
                    Opcodes.AND -> binaryIntOp(stack) { a, b -> a and b }
                    Opcodes.OR -> binaryIntOp(stack) { a, b -> a or b }
                    Opcodes.XOR -> binaryIntOp(stack) { a, b -> a xor b }
                    Opcodes.SHL -> binaryIntOp(stack) { a, b -> a shl b.toInt() }
                    Opcodes.SHR -> binaryIntOp(stack) { a, b -> a shr b.toInt() }

                    Opcodes.NEG -> {
                        val a = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                        stack.addLast(GMLValue.of(-a.toReal()))
                    }
                    Opcodes.NOT -> {
                        val a = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                        stack.addLast(GMLValue.of(if (a.toReal().toLong() != 0L) 0.0 else 1.0))
                    }

                    Opcodes.CMP -> {
                        val b = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                        val a = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                        val cmpType = instr.comparisonType
                        val result = if (a is GMLValue.Str && b is GMLValue.Str) {
                            val cmp = a.value.compareTo(b.value)
                            compareBool(cmp, cmpType)
                        } else {
                            val aR = a.toReal()
                            val bR = b.toReal()
                            // Use == first to handle -0.0 == 0.0 correctly
                            // (Double.compareTo distinguishes them: (-0.0).compareTo(0.0) == -1)
                            val cmp = if (aR == bR) 0 else aR.compareTo(bR)
                            compareBool(cmp, cmpType)
                        }
                        stack.addLast(GMLValue.of(result))
                    }

                    Opcodes.CONV -> {
                        // Type conversion: just leave value on stack, the VM handles types dynamically
                    }

                    Opcodes.B -> {
                        val offset = instr.branchOffset
                        pc = findBranchTarget(decoded, pc - 1, offset)
                    }
                    Opcodes.BT -> {
                        val cond = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                        if (cond.toBool()) {
                            pc = findBranchTarget(decoded, pc - 1, instr.branchOffset)
                        }
                    }
                    Opcodes.BF -> {
                        val cond = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                        if (!cond.toBool()) {
                            pc = findBranchTarget(decoded, pc - 1, instr.branchOffset)
                        }
                    }

                    Opcodes.PUSHENV -> {
                        val target = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                        val targetId = target.toInt()
                        val instances = runner.findInstancesByObjectOrId(targetId)
                        if (instances.isEmpty()) {
                            // Skip to matching PopEnv
                            pc = findBranchTarget(decoded, pc - 1, instr.branchOffset)
                        } else {
                            envIterStack.addLast(EnvIteration(instances, 0, currentSelf, currentOther))
                            currentOther = currentSelf
                            currentSelf = instances[0]
                        }
                    }

                    Opcodes.POPENV -> {
                        if (envIterStack.isNotEmpty()) {
                            val iter = envIterStack.last()
                            iter.currentIdx++
                            // Skip destroyed instances
                            while (iter.currentIdx < iter.instances.size && iter.instances[iter.currentIdx].destroyed) {
                                iter.currentIdx++
                            }
                            if (iter.currentIdx < iter.instances.size) {
                                // More instances: loop back to body start
                                currentSelf = iter.instances[iter.currentIdx]
                                pc = findBranchTarget(decoded, pc - 1, instr.branchOffset)
                            } else {
                                // Done: restore context
                                val finished = envIterStack.removeLast()
                                currentSelf = finished.prevSelf
                                currentOther = finished.prevOther
                            }
                        }
                    }

                    Opcodes.CALL -> {
                        val argc = instr.argCount
                        // GM:S bytecode pushes args in reverse order (last arg first),
                        // so popping gives us args in correct order already
                        val args = (0 until argc).map {
                            if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
                        }

                        val funcIdx = instr.functionIndex
                        val result = if (funcIdx >= 0) {
                            val funcName = gameData.functions[funcIdx].name
                            callFunction(funcName, args, currentSelf, currentOther, locals)
                        } else {
                            GMLValue.ZERO
                        }
                        stack.addLast(result)
                    }

                    Opcodes.RET -> {
                        // Pop the return value from the stack and store it for the caller
                        if (stack.isNotEmpty()) {
                            self.returnValue = stack.removeLast()
                        }
                        return
                    }

                    Opcodes.EXIT -> {
                        return
                    }

                    else -> {
                        // Unknown opcode, skip
                    }
                }
            } catch (e: Exception) {
                val entryName = gameData.codeEntries[codeEntryIndex].name
                println("VM ERROR in $entryName at pc=${pc - 1}, opcode=0x${instr.opcode.toString(16)}: ${e.message}")
                e.printStackTrace()
                if (tracing) traceCodeEntry = prevTrace
                return
            }
        }
        if (tracing) traceCodeEntry = prevTrace
    }

    private fun binaryOp(stack: ArrayDeque<GMLValue>, op: (Double, Double) -> Double) {
        val b = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
        val a = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
        stack.addLast(GMLValue.of(op(a.toReal(), b.toReal())))
    }

    private fun binaryIntOp(stack: ArrayDeque<GMLValue>, op: (Long, Long) -> Long) {
        val b = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
        val a = if (stack.isNotEmpty()) stack.removeLast() else GMLValue.ZERO
        stack.addLast(GMLValue.of(op(a.toLong(), b.toLong()).toDouble()))
    }

    private fun compareBool(cmp: Int, cmpType: Int): Boolean = when (cmpType) {
        ComparisonTypes.LT -> cmp < 0
        ComparisonTypes.LTE -> cmp <= 0
        ComparisonTypes.EQ -> cmp == 0
        ComparisonTypes.NEQ -> cmp != 0
        ComparisonTypes.GTE -> cmp >= 0
        ComparisonTypes.GT -> cmp > 0
        else -> false
    }

    // Reverse map: instruction index -> bytecode offset (built lazily)
    private val indexToOffsetCache = mutableMapOf<DecodedCode, IntArray>()

    private var branchDebugCount = 0
    private fun findBranchTarget(decoded: DecodedCode, currentInstrIndex: Int, branchOffset: Int): Int {
        // Build reverse map (instruction index -> bytecode offset) if not cached
        val reverseMap = indexToOffsetCache.getOrPut(decoded) {
            val arr = IntArray(decoded.instructions.size)
            for ((offset, idx) in decoded.offsetToIndex) {
                if (idx < arr.size) arr[idx] = offset
            }
            arr
        }

        val currentBytecodeOffset = if (currentInstrIndex < reverseMap.size) reverseMap[currentInstrIndex] else return decoded.instructions.size
        val targetOffset = currentBytecodeOffset + branchOffset * 4
        val result = decoded.offsetToIndex[targetOffset]
        if (result == null && branchDebugCount < 5) {
            branchDebugCount++
            val entryIdx = decodedEntries.indexOf(decoded)
            val entryName = if (entryIdx >= 0 && entryIdx < gameData.codeEntries.size) gameData.codeEntries[entryIdx].name else "?"
            println("  [BRANCH DEBUG] $entryName: pc=$currentInstrIndex branchOffset=$branchOffset currentByteOff=$currentBytecodeOffset targetByteOff=$targetOffset NOT FOUND in offsetToIndex (${decoded.offsetToIndex.size} entries, maxOffset=${decoded.offsetToIndex.keys.maxOrNull()}, instrCount=${decoded.instructions.size})")
        }
        return result ?: decoded.instructions.size // Past end = exit code
    }

    private fun resolveInstance(instType: Int, self: Instance, other: Instance): Instance? = when (instType) {
        InstanceTypes.SELF, InstanceTypes.BUILTIN -> self
        InstanceTypes.OTHER -> other
        InstanceTypes.GLOBAL -> null // handled separately
        InstanceTypes.LOCAL -> null  // handled separately
        else -> {
            if (instType >= 0) {
                // Object ID - find first instance of this object
                runner.findFirstInstanceByObject(instType)
            } else self
        }
    }

    fun getGlobalBuiltin(name: String): GMLValue {
        val r = runner
        return when (name) {
            "room" -> GMLValue.of(r.currentRoomIndex.toDouble())
            "room_speed" -> GMLValue.of(r.currentRoom?.speed?.toDouble() ?: 30.0)
            "room_width" -> GMLValue.of(r.currentRoom?.width?.toDouble() ?: 640.0)
            "room_height" -> GMLValue.of(r.currentRoom?.height?.toDouble() ?: 480.0)
            "view_current" -> GMLValue.of(r.renderer.currentView.toDouble())
            "current_time" -> GMLValue.of(System.currentTimeMillis().toDouble())
            "fps" -> GMLValue.of(r.fps.toDouble())
            "instance_count" -> GMLValue.of(r.instances.size.toDouble())
            "keyboard_key" -> GMLValue.of(r.keyboardKey.toDouble())
            "keyboard_lastkey" -> GMLValue.of(r.keyboardLastKey.toDouble())
            "mouse_x" -> GMLValue.of(r.mouseX)
            "mouse_y" -> GMLValue.of(r.mouseY)
            "os_type" -> GMLValue.of(1.0) // os_windows
            "game_id" -> GMLValue.of(gameData.gen8.gameId.toDouble())
            "working_directory" -> GMLValue.of("")
            "program_directory" -> GMLValue.of("")
            "temp_directory" -> GMLValue.of("/tmp/")
            "browser_width" -> GMLValue.of(gameData.gen8.windowWidth.toDouble())
            "browser_height" -> GMLValue.of(gameData.gen8.windowHeight.toDouble())
            "room_persistent" -> GMLValue.of(r.roomPersistentFlags[r.currentRoomIndex] == true)
            "display_aa" -> GMLValue.ZERO
            "application_surface" -> GMLValue.of(-1.0) // sentinel for application surface
            // Path end action constants
            "path_action_stop" -> GMLValue.of(0.0)
            "path_action_restart" -> GMLValue.of(1.0)
            "path_action_continue" -> GMLValue.of(2.0)
            "path_action_reverse" -> GMLValue.of(3.0)
            else -> r.globalVariables[name] ?: GMLValue.ZERO
        }
    }

    private fun traceGlobalWrite(name: String, value: GMLValue, codeName: String?) {
        if (Butterscotch.traceGlobalVars.isEmpty())
            return

        if ("*" !in Butterscotch.traceGlobalVars && name !in Butterscotch.traceGlobalVars)
            return

        val r = this.runner
        val old = r.globalVariables[name] ?: GMLValue.ZERO
        val code = codeName ?: "builtin"
        println("  [TRACE] global.$name = $value (was $old) at frame=${r.frameCount}, code=$code")
    }

    private fun traceInstanceVarWrite(target: Instance, varName: String, newValue: GMLValue, arrayIdx: Int, codeName: String?) {
        if (Butterscotch.traceInstanceVars.isEmpty())
            return

        val objectName = if (target.objectIndex in gameData.objects.indices) gameData.objects[target.objectIndex].name else "unknown"

        // Check filters
        val matched = Butterscotch.traceInstanceVars.any { filter ->
            when {
                filter == "*" -> true
                "." in filter -> {
                    val (objPart, varPart) = filter.split(".", limit = 2)
                    (objPart == objectName) && (varPart == "*" || varPart == varName)
                }
                else -> varName == filter
            }
        }
        if (!matched) return

        val r = this.runner
        val old = if (arrayIdx >= 0) target.getArrayElement(varName, arrayIdx) else target.getBuiltinOrVar(varName)
        val code = codeName ?: "builtin"
        val varDisplay = if (arrayIdx >= 0) "$varName[$arrayIdx]" else varName
        println("  [TRACE] $objectName(id=${target.id}).$varDisplay = $newValue (was $old) at frame=${r.frameCount}, code=$code")
    }

    /**
     * Returns a value for builtin array variables (view_wview, view_hview, etc.)
     * Returns null if the variable isn't a known builtin array.
     */
    private fun getBuiltinArrayElement(name: String, index: Int): GMLValue? {
        val r = runner
        val room = r.currentRoom
        return when (name) {
            "view_wview" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.viewW?.toDouble() ?: 640.0)
            }
            "view_hview" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.viewH?.toDouble() ?: 480.0)
            }
            "view_xview" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.viewX?.toDouble() ?: 0.0)
            }
            "view_yview" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.viewY?.toDouble() ?: 0.0)
            }
            "view_wport" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.portW?.toDouble() ?: 640.0)
            }
            "view_hport" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.portH?.toDouble() ?: 480.0)
            }
            "view_visible" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.enabled == true)
            }
            "view_xport" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.portX?.toDouble() ?: 0.0)
            }
            "view_yport" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.portY?.toDouble() ?: 0.0)
            }
            "view_hborder" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.borderH?.toDouble() ?: 0.0)
            }
            "view_vborder" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.borderV?.toDouble() ?: 0.0)
            }
            "view_hspeed" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.speedH?.toDouble() ?: 0.0)
            }
            "view_vspeed" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.speedV?.toDouble() ?: 0.0)
            }
            "view_object" -> {
                val view = room?.views?.getOrNull(index)
                GMLValue.of(view?.followObjectId?.toDouble() ?: -1.0)
            }
            else -> null // Not a known builtin array
        }
    }

    private fun setBuiltinArrayElement(name: String, index: Int, value: GMLValue): Boolean {
        val room = runner.currentRoom ?: return false
        val view = room.views.getOrNull(index) ?: return false
        when (name) {
            "view_xview" -> view.viewX = value.toReal().toInt()
            "view_yview" -> view.viewY = value.toReal().toInt()
            "view_wview" -> view.viewW = value.toReal().toInt()
            "view_hview" -> view.viewH = value.toReal().toInt()
            "view_wport" -> view.portW = value.toReal().toInt()
            "view_hport" -> view.portH = value.toReal().toInt()
            "view_xport" -> view.portX = value.toReal().toInt()
            "view_yport" -> view.portY = value.toReal().toInt()
            "view_hborder" -> view.borderH = value.toReal().toInt()
            "view_vborder" -> view.borderV = value.toReal().toInt()
            "view_hspeed" -> view.speedH = value.toReal().toInt()
            "view_vspeed" -> view.speedV = value.toReal().toInt()
            "view_object" -> view.followObjectId = value.toReal().toInt()
            "view_visible" -> view.enabled = value.toReal() > 0.5
            else -> return false
        }
        return true
    }

    private fun getGlobalArrayElement(name: String, index: Int): GMLValue {
        val arr = runner.globalVariables[name]
        return if (arr is GMLValue.ArrayVal) arr.data[0]?.get(index) ?: GMLValue.ZERO else GMLValue.ZERO
    }

    private fun setGlobalArrayElement(name: String, index: Int, value: GMLValue) {
        val existing = runner.globalVariables[name]
        val arr = if (existing is GMLValue.ArrayVal) existing else {
            val newArr = GMLValue.ArrayVal()
            runner.globalVariables[name] = newArr
            newArr
        }
        arr.data.getOrPut(0) { mutableMapOf() }[index] = value
    }

    var traceCodeEntry = ""
    private val unknownFunctions = mutableSetOf<String>()

    // Current execution context (set before calling builtins so they can access self/other)
    var currentSelf: Instance? = null
    var currentOther: Instance? = null

    fun callFunction(name: String, args: List<GMLValue>, self: Instance, other: Instance, locals: MutableMap<Int, GMLValue>): GMLValue {
        if (Butterscotch.traceCalls.isNotEmpty() && name !in Butterscotch.ignoreFunctionTracedCalls) {
            val objectData = self.getObjectData(this)
            if (Butterscotch.traceCalls.contains("*") || objectData.name in Butterscotch.traceCalls) {
                println("  CALL (${objectData.name}): $name(${args.joinToString { it.toStr().take(30) }})")
            }
        }

        // Set execution context so builtins can access self/other
        val prevSelf = currentSelf
        val prevOther = currentOther
        currentSelf = self
        currentOther = other

        // Check builtin functions first
        val builtin = builtinFunctions[name]
        if (builtin != null) {
            val result = builtin(this, args)
            currentSelf = prevSelf
            currentOther = prevOther
            return result
        }

        // Check user scripts
        val codeId = scriptNameToCodeId[name]
        if (codeId != null) {
            // Pass args via self.argument0..argument15
            for ((i, arg) in args.withIndex()) {
                self.variables["argument$i"] = arg
            }
            self.variables["argument_count"] = GMLValue.of(args.size.toDouble())
            // Also create argument array for argument[n] access
            val argArray = GMLValue.ArrayVal()
            val argMap = mutableMapOf<Int, GMLValue>()
            for ((i, arg) in args.withIndex()) {
                argMap[i] = arg
            }
            argArray.data[0] = argMap
            self.variables["argument"] = argArray

            // Clear return value before execution to avoid stale values
            self.returnValue = null

            executeCode(codeId, self, other)

            currentSelf = prevSelf
            currentOther = prevOther

            return self.returnValue ?: GMLValue.ZERO
        }

        currentSelf = prevSelf
        currentOther = prevOther

        // Unknown function - stub (only warn once per function per object)
        val objectData = self.getObjectData(this)
        val unknownFunctionKey = "${objectData.name}:${name}"

        if (Butterscotch.alwaysLogUnknownInstructions || unknownFunctionKey !in unknownFunctions) {
            unknownFunctions.add(unknownFunctionKey)
            println("WARNING: Unknown function '$name' with ${args.size} args (called by ${objectData.name})")
        }

        return GMLValue.ZERO
    }
}
