/*
    enoki/llvm.h -- LLVM-backed Enoki dynamic array with JIT compilation

    Enoki is a C++ template library for efficient vectorization and
    differentiation of numerical kernels on modern processor architectures.

    Copyright (c) 2020 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#pragma once

#include <enoki/array.h>
#include <enoki-jit/jit.h>
#include <enoki-jit/traits.h>

NAMESPACE_BEGIN(enoki)

template <typename Value_>
struct LLVMArray : ArrayBaseT<Value_, is_mask_v<Value_>, LLVMArray<Value_>> {
    template <typename> friend struct LLVMArray;

    static_assert(std::is_scalar_v<Value_>,
                  "LLVM Arrays can only be created over scalar types!");

    // -----------------------------------------------------------------------
    //! @{ \name Basic type declarations
    // -----------------------------------------------------------------------

    using Value = Value_;
    using MaskType = LLVMArray<bool>;
    using ArrayType = LLVMArray;

    static constexpr bool IsLLVM = true;
    static constexpr bool IsJIT = true;
    static constexpr bool IsDynamic = true;
    static constexpr size_t Size = Dynamic;
    static constexpr bool IsClass =
        std::is_pointer_v<Value_> &&
        std::is_class_v<std::remove_pointer_t<Value_>>;

    static constexpr VarType Type =
        IsClass ? VarType::UInt32 : var_type_v<Value>;

    using ActualValue = std::conditional_t<IsClass, uint32_t, Value>;
    using CallSupport =
        call_support<std::decay_t<std::remove_pointer_t<Value_>>, LLVMArray>;

    template <typename T> using ReplaceValue = LLVMArray<T>;

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Constructors and assignment operators
    // -----------------------------------------------------------------------

    LLVMArray() = default;

    ~LLVMArray() { jitc_var_dec_ref_ext(m_index); }

    LLVMArray(const LLVMArray &a) : m_index(a.m_index) {
        jitc_var_inc_ref_ext(m_index);
    }

    LLVMArray(LLVMArray &&a) noexcept : m_index(a.m_index) {
        a.m_index = 0;
    }

    template <typename T> LLVMArray(const LLVMArray<T> &v) {
        static_assert(!std::is_same_v<T, Value>,
                      "Conversion constructor called with arguments that don't "
                      "correspond to a conversion!");
        const char *op;
        if constexpr (std::is_floating_point_v<Value> && std::is_integral_v<T>) {
            op = std::is_signed_v<T> ? "$r0 = sitofp <$w x $t1> $r1 to <$w x $t0>"
                                     : "$r0 = uitofp <$w x $t1> $r1 to <$w x $t0>";
        } else if constexpr (std::is_integral_v<Value> && std::is_floating_point_v<T>) {
            op = std::is_signed_v<Value>? "$r0 = fptosi <$w x $t1> $r1 to <$w x $t0>"
                                        : "$r0 = fptoui <$w x $t1> $r1 to <$w x $t0>";
        } else if constexpr (std::is_floating_point_v<T> && std::is_floating_point_v<Value>) {
            op = sizeof(T) > sizeof(Value) ? "$r0 = fptrunc <$w x $t1> $r1 to <$w x $t0>"
                                           : "$r0 = fpext <$w x $t1> $r1 to <$w x $t0>";
        } else if constexpr (std::is_integral_v<T> && std::is_integral_v<Value>) {
            constexpr size_t size_1 = std::is_same_v<T,     bool> ? 0 : sizeof(T),
                             size_2 = std::is_same_v<Value, bool> ? 0 : sizeof(Value);

            if constexpr (size_1 == size_2) {
                m_index = v.index();
                jitc_var_inc_ref_ext(m_index);
                return;
            } else {
                op = size_1 > size_2
                         ? "$r0 = trunc <$w x $t1> $r1 to <$w x $t0>"
                         : (std::is_signed_v<T>
                                ? "$r0 = sext <$w x $t1> $r1 to <$w x $t0>"
                                : "$r0 = zext <$w x $t1> $r1 to <$w x $t0>");
            }
        } else {
            jitc_fail("Unsupported conversion!");
        }

        m_index = jitc_var_new_1(Type, op, 1, 0, v.index());
    }

    template <typename T> LLVMArray(const LLVMArray<T> &v, detail::reinterpret_flag) {
        static_assert(
            sizeof(T) == sizeof(Value),
            "reinterpret_array requires arrays with equal-sized element types!");

        if constexpr (std::is_integral_v<Value> != std::is_integral_v<T>) {
            m_index = jitc_var_new_1(
                Type, "$r0 = bitcast <$w x $t1> $r1 to <$w x $t0>", 1, 0,
                v.index());
        } else {
            m_index = v.index();
            jitc_var_inc_ref_ext(m_index);
        }
    }

    LLVMArray(Value value) {
        if constexpr (!IsClass) {
            uint64_t tmp = 0;
            memcpy(&tmp, &value, sizeof(Value));
            m_index = jitc_var_new_literal(Type, 0, tmp, 1);
        } else {
            m_index = jitc_var_new_literal(
                Type, 0, (uint64_t) jitc_registry_get_id(value), 1);
        }
    }

    template <typename... Ts, enable_if_t<(sizeof...(Ts) > 1 &&
              (!std::is_same_v<Ts, detail::reinterpret_flag> && ...))> = 0>
    LLVMArray(Ts&&... ts) {
        if constexpr (!IsClass) {
            Value data[] = { (Value) ts... };
            m_index = jitc_var_copy(AllocType::Host, Type, 0, data,
                                    (uint32_t) sizeof...(Ts));
        } else {
            uint32_t data[] = { jitc_registry_get_id(ts)... };
            m_index = jitc_var_copy(AllocType::Host, Type, 0, data,
                                    (uint32_t) sizeof...(Ts));
        }
    }

    LLVMArray &operator=(const LLVMArray &a) {
        jitc_var_inc_ref_ext(a.m_index);
        jitc_var_dec_ref_ext(m_index);
        m_index = a.m_index;
        return *this;
    }

    LLVMArray &operator=(LLVMArray &&a) {
        std::swap(m_index, a.m_index);
        return *this;
    }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Vertical operations
    // -----------------------------------------------------------------------

    LLVMArray add_(const LLVMArray &v) const {
        if constexpr (!jitc_is_arithmetic(Type))
            enoki_raise("Unsupported operand type");

        // Simple constant propagation
        if (is_literal_zero())
            return v;
        else if (v.is_literal_zero())
            return *this;

        const char *op = std::is_floating_point_v<Value>
            ? "$r0 = fadd <$w x $t0> $r1, $r2"
            : "$r0 = add <$w x $t0> $r1, $r2";

        return from_index(jitc_var_new_2(Type, op, 1, 0, m_index, v.m_index));
    }

    LLVMArray sub_(const LLVMArray &v) const {
        if constexpr (!jitc_is_arithmetic(Type))
            enoki_raise("Unsupported operand type");

        const char *op = std::is_floating_point_v<Value>
            ? "$r0 = fsub <$w x $t0> $r1, $r2"
            : "$r0 = sub <$w x $t0> $r1, $r2";


        return from_index(jitc_var_new_2(Type, op, 1, 0, m_index, v.m_index));
    }

    LLVMArray mul_(const LLVMArray &v) const {
        if constexpr (!jitc_is_arithmetic(Type))
            enoki_raise("Unsupported operand type");

        // Simple constant propagation
        if (is_literal_one())
            return v;
        else if (v.is_literal_one())
            return *this;

        const char *op = std::is_floating_point_v<Value>
            ? "$r0 = fmul <$w x $t0> $r1, $r2"
            : "$r0 = mul <$w x $t0> $r1, $r2";

        return from_index(jitc_var_new_2(Type, op, 1, 0, m_index, v.m_index));
    }

    LLVMArray div_(const LLVMArray &v) const {
        if constexpr (!jitc_is_arithmetic(Type))
            enoki_raise("Unsupported operand type");

        // Simple constant propagation
        if (v.is_literal_one())
            return *this;

        const char *op;
        if constexpr (std::is_floating_point_v<Value>)
            op = "$r0 = fdiv <$w x $t0> $r1, $r2";
        else if constexpr (std::is_signed_v<Value>)
            op = "$r0 = sdiv <$w x $t0> $r1, $r2";
        else
            op = "$r0 = udiv <$w x $t0> $r1, $r2";

        return from_index(jitc_var_new_2(Type, op, 1, 0, m_index, v.m_index));
    }

    LLVMArray mod_(const LLVMArray &v) const {
        if constexpr (!jitc_is_integral(Type))
            enoki_raise("Unsupported operand type");

        const char *op;
        if constexpr (std::is_signed_v<Value>)
            op = "$r0 = srem <$w x $t0> $r1, $r2";
        else
            op = "$r0 = urem <$w x $t0> $r1, $r2";

        return from_index(jitc_var_new_2(Type, op, 1, 0, m_index, v.m_index));
    }

    LLVMArray<bool> gt_(const LLVMArray &a) const {
        if constexpr (!jitc_is_arithmetic(Type))
            enoki_raise("Unsupported operand type");

        const char *op;
        if constexpr (std::is_integral_v<Value>)
            op = std::is_signed_v<Value>
                     ? "$r0 = icmp sgt <$w x $t1> $r1, $r2"
                     : "$r0 = icmp ugt <$w x $t1> $r1, $r2";
        else
            op = "$r0 = fcmp ogt <$w x $t1> $r1, $r2";

        return LLVMArray<bool>::from_index(jitc_var_new_2(
            LLVMArray<bool>::Type, op, 1, 0, m_index, a.index()));
    }

    LLVMArray<bool> ge_(const LLVMArray &a) const {
        if constexpr (!jitc_is_arithmetic(Type))
            enoki_raise("Unsupported operand type");

        const char *op;
        if constexpr (std::is_integral_v<Value>)
            op = std::is_signed_v<Value>
                     ? "$r0 = icmp sge <$w x $t1> $r1, $r2"
                     : "$r0 = icmp uge <$w x $t1> $r1, $r2";
        else
            op = "$r0 = fcmp oge <$w x $t1> $r1, $r2";

        return LLVMArray<bool>::from_index(jitc_var_new_2(
            LLVMArray<bool>::Type, op, 1, 0, m_index, a.index()));
    }


    LLVMArray<bool> lt_(const LLVMArray &a) const {
        if constexpr (!jitc_is_arithmetic(Type))
            enoki_raise("Unsupported operand type");

        const char *op;
        if constexpr (std::is_integral_v<Value>)
            op = std::is_signed_v<Value>
                     ? "$r0 = icmp slt <$w x $t1> $r1, $r2"
                     : "$r0 = icmp ult <$w x $t1> $r1, $r2";
        else
            op = "$r0 = fcmp olt <$w x $t1> $r1, $r2";

        return LLVMArray<bool>::from_index(jitc_var_new_2(
            LLVMArray<bool>::Type, op, 1, 0, m_index, a.index()));
    }

    LLVMArray<bool> le_(const LLVMArray &a) const {
        if constexpr (!jitc_is_arithmetic(Type))
            enoki_raise("Unsupported operand type");

        const char *op;
        if constexpr (std::is_integral_v<Value>)
            op = std::is_signed_v<Value>
                     ? "$r0 = icmp sle <$w x $t1> $r1, $r2"
                     : "$r0 = icmp ule <$w x $t1> $r1, $r2";
        else
            op = "$r0 = fcmp ole <$w x $t1> $r1, $r2";

        return LLVMArray<bool>::from_index(jitc_var_new_2(
            LLVMArray<bool>::Type, op, 1, 0, m_index, a.index()));
    }

    LLVMArray<bool> eq_(const LLVMArray &b) const {
        const char *op = std::is_integral_v<Value>
                             ? "$r0 = icmp eq <$w x $t1> $r1, $r2"
                             : "$r0 = fcmp oeq <$w x $t1> $r1, $r2";

        return LLVMArray<bool>::from_index(jitc_var_new_2(
            LLVMArray<bool>::Type, op, 1, 0, m_index, b.index()));
    }

    LLVMArray<bool> neq_(const LLVMArray &b) const {
        const char *op = std::is_integral_v<Value>
                             ? "$r0 = icmp ne <$w x $t1> $r1, $r2"
                             : "$r0 = fcmp one <$w x $t1> $r1, $r2";

        return LLVMArray<bool>::from_index(jitc_var_new_2(
            LLVMArray<bool>::Type, op, 1, 0, m_index, b.index()));
    }

    LLVMArray neg_() const {
        if constexpr (!jitc_is_arithmetic(Type))
            enoki_raise("Unsupported operand type");

        const char *op;
        if constexpr (std::is_floating_point_v<Value>) {
            if (jitc_llvm_version_major() > 7)
                op = "$r0 = fneg <$w x $t0> $r1";
            else
                op = "$r0 = fsub <$w x $t0> zeroinitializer, $r1";
        } else {
            op = "$r0 = sub <$w x $t0> $z, $r1";
        }

        return from_index(jitc_var_new_1(Type, op, 1, 0, m_index));
    }

    LLVMArray not_() const {
        if constexpr (std::is_same_v<Value, bool>) {
            if (is_literal_one())
                return LLVMArray(false);
            else if (is_literal_zero())
                return LLVMArray(true);
        }

        const char *op = std::is_integral_v<Value>
                             ? "$r0 = xor <$w x $t1> $r1, $o0"
                             : "$r0_0 = bitcast <$w x $t1> $r1 to <$w x $b0>$n"
                               "$r0_1 = xor <$w x $b0> $r0_0, $o0$n"
                               "$r0 = bitcast <$w x $b0> $r0_1 to <$w x $t0>";
        return from_index(jitc_var_new_1(Type, op, 1, 0, m_index));
    }

    template <typename T> LLVMArray or_(const T &a) const {
        if constexpr (std::is_same_v<T, LLVMArray>) {
            // Simple constant propagation
            if constexpr (std::is_same_v<Value, bool>) {
                if (is_literal_one() || a.is_literal_zero())
                    return *this;
                else if (a.is_literal_one() || is_literal_zero())
                    return a;
            }

            const char *op = std::is_integral_v<Value>
                                 ? "$r0 = or <$w x $t1> $r1, $r2"
                                 : "$r0_0 = bitcast <$w x $t1> $r1 to <$w x $b0>$n"
                                   "$r0_1 = bitcast <$w x $t2> $r2 to <$w x $b0>$n"
                                   "$r0_2 = or <$w x $b0> $r0_0, $r0_1"
                                   "$r0 = bitcast <$w x $b0> $r0_2 to <$w x $t0>";

            return from_index(jitc_var_new_2(Type, op, 1, 0,
                                             m_index, a.index()));
        } else {
            // Simple constant propagation
            if (a.is_literal_zero())
                return *this;
            else if (a.is_literal_one())
                return LLVMArray(memcpy_cast<Value>(int_array_t<Value>(-1)));

            using UInt = uint_array_t<LLVMArray>;
            UInt x = UInt::from_index(jitc_var_new_1(
                UInt::Type, "$r0 = sext <$w x $t1> $r1 to <$w x $b0>", 1, 0,
                a.index()));

            return *this | reinterpret_array<LLVMArray>(x);
        }
    }

    template <typename T> LLVMArray and_(const T &a) const {
        if constexpr (std::is_same_v<T, LLVMArray>) {
            // Simple constant propagation
            if constexpr (std::is_same_v<Value, bool>) {
                if (is_literal_one() || a.is_literal_zero())
                    return a;
                else if (a.is_literal_one() || is_literal_zero())
                    return *this;
            }

            const char *op = std::is_integral_v<Value>
                                 ? "$r0 = and <$w x $t1> $r1, $r2"
                                 : "$r0_0 = bitcast <$w x $t1> $r1 to <$w x $b0>$n"
                                   "$r0_1 = bitcast <$w x $t2> $r2 to <$w x $b0>$n"
                                   "$r0_2 = and <$w x $b0> $r0_0, $r0_1"
                                   "$r0 = bitcast <$w x $b0> $r0_2 to <$w x $t0>";

            return from_index(jitc_var_new_2(Type, op, 1, 0, m_index, a.index()));
        } else {
            // Simple constant propagation
            if (a.is_literal_one())
                return *this;
            else if (a.is_literal_zero())
                return LLVMArray(Value(0));

            using UInt = uint_array_t<LLVMArray>;
            UInt x = UInt::from_index(jitc_var_new_1(
                UInt::Type, "$r0 = sext <$w x $t1> $r1 to <$w x $b0>", 1, 0,
                a.index()));

            return *this & reinterpret_array<LLVMArray>(x);
        }
    }

    template <typename T> LLVMArray xor_(const T &a) const {
        if constexpr (std::is_same_v<T, LLVMArray>) {
            // Simple constant propagation
            if constexpr (std::is_same_v<Value, bool>) {
                if (is_literal_zero())
                    return a;
                else if (a.is_literal_zero())
                    return *this;
            }

            const char *op = std::is_integral_v<Value>
                                 ? "$r0 = xor <$w x $t1> $r1, $r2"
                                 : "$r0_0 = bitcast <$w x $t1> $r1 to <$w x $b0>$n"
                                   "$r0_1 = bitcast <$w x $t2> $r2 to <$w x $b0>$n"
                                   "$r0_2 = xor <$w x $b0> $r0_0, $r0_1$n"
                                   "$r0 = bitcast <$w x $b0> $r0_2 to <$w x $t0>";

            return from_index(jitc_var_new_2(Type, op, 1, 0, m_index, a.index()));
        } else {
            // Simple constant propagation
            if (a.is_literal_zero())
                return *this;

            using UInt = uint_array_t<LLVMArray>;
            UInt x = UInt::from_index(jitc_var_new_1(
                UInt::Type, "$r0 = sext <$w x $t1> $r1 to <$w x $b0>", 1, 0,
                a.index()));

            return *this ^ reinterpret_array<LLVMArray>(x);
        }
    }

    template <typename T> LLVMArray andnot_(const T &a) const {
        return and_(a.not_());
    }

    template <int Imm> LLVMArray sl_() const {
        return sl_((uint32_t) Imm);
    }

    LLVMArray sl_(const LLVMArray<uint_array_t<Value>> &v) const {
        if constexpr (!jitc_is_integral(Type))
            enoki_raise("Unsupported operand type");

        return from_index(jitc_var_new_2(
            Type, "$r0 = shl <$w x $t0> $r1, $r2", 1, 0, m_index, v.index()));
    }

    template <int Imm> LLVMArray sr_() const {
        return sr_((uint32_t) Imm);
    }

    LLVMArray sr_(const LLVMArray<uint_array_t<Value>> &v) const {
        if constexpr (!jitc_is_integral(Type))
            enoki_raise("Unsupported operand type");

        const char *op;
        if constexpr (std::is_signed_v<Value>)
            op = "$r0 = ashr <$w x $t0> $r1, $r2";
        else
            op = "$r0 = lshr <$w x $t0> $r1, $r2";

        return from_index(jitc_var_new_2(Type, op, 1, 0, m_index, v.index()));
    }

    LLVMArray abs_() const {
        if constexpr (!jitc_is_arithmetic(Type))
            enoki_raise("Unsupported operand type");

        if constexpr (std::is_floating_point_v<Value>)
            return LLVMArray<Value>(detail::not_(detail::sign_mask<Value>())) & *this;
        else
            return select(*this > 0, *this, -*this);
    }

    LLVMArray sqrt_() const {
        if constexpr (!jitc_is_floating_point(Type))
            enoki_raise("Unsupported operand type");

        return from_index(jitc_var_new_1(
            Type, "$r0 = call <$w x $t0> @llvm.sqrt.v$w$a1(<$w x $t1> $r1)", 1, 0,
            m_index));
    }

    LLVMArray rcp_() const {
        return Value(1) / *this;
    }

    LLVMArray rsqrt_() const {
        return sqrt(Value(1) / *this);
    }

    LLVMArray min_(const LLVMArray &a) const {
        // Portable intrinsic as a last resort
        const char *op = "$r0 = call <$w x $t0> @llvm.minnum.v$w$a1(<$w x $t1> "
                         "$r1, <$w x $t2> $r2)";

        // Prefer an X86-specific intrinsic (produces nicer machine code)
        if constexpr (std::is_integral_v<Value>) {
            (void) op;
            return select(*this < a, *this, a);
        } else if constexpr (std::is_same_v<Value, float>) {
            if (jitc_llvm_if_at_least(16, "+avx512f")) {
                op = "$4$r0 = call <$w x $t0> @llvm.x86.avx512.min.ps.512(<$w x $t1> "
                     "$r1, <$w x $t2> $r2, i32$S 4)";
            } else if (jitc_llvm_if_at_least(8, "+avx")) {
                op = "$3$r0 = call <$w x $t0> @llvm.x86.avx.min.ps.256(<$w x $t1> "
                     "$r1, <$w x $t2> $r2)";
            } else if (jitc_llvm_if_at_least(4, "+sse4.2")) {
                op = "$2$r0 = call <$w x $t0> @llvm.x86.sse.min.ps(<$w x $t1> $r1, "
                     "<$w x $t2> $r2)";
            }
        } else if (std::is_same_v<Value, double>) {
            if (jitc_llvm_if_at_least(8, "+avx512f")) {
                op = "$3$r0 = call <$w x $t0> @llvm.x86.avx512.min.pd.512(<$w x $t1> "
                     "$r1, <$w x $t2> $r2, i32$S 4)";
            } else if (jitc_llvm_if_at_least(4, "+avx")) {
                op = "$2$r0 = call <$w x $t0> @llvm.x86.avx.min.pd.256(<$w x $t1> "
                     "$r1, <$w x $t2> $r2)";
            } else if (jitc_llvm_if_at_least(2, "+sse4.2")) {
                op = "$1$r0 = call <$w x $t0> @llvm.x86.sse.min.pd(<$w x $t1> $r1, "
                     "<$w x $t2> $r2)";
            }
        }

        return from_index(jitc_var_new_2(Type, op, 1, 0, m_index, a.index()));
    }

    LLVMArray max_(const LLVMArray &a) const {
        // Portable intrinsic as a last resort
        const char *op = "$r0 = call <$w x $t0> @llvm.maxnum.v$w$a1(<$w x $t1> "
                         "$r1, <$w x $t2> $r2)";

        // Prefer an X86-specific intrinsic (produces nicer machine code)
        if constexpr (std::is_integral_v<Value>) {
            (void) op;
            return select(*this < a, a, *this);
        } else if constexpr (std::is_same_v<Value, float>) {
            if (jitc_llvm_if_at_least(16, "+avx512f")) {
                op = "$4$r0 = call <$w x $t0> @llvm.x86.avx512.max.ps.512(<$w x $t1> "
                     "$r1, <$w x $t2> $r2, i32$S 4)";
            } else if (jitc_llvm_if_at_least(8, "+avx")) {
                op = "$3$r0 = call <$w x $t0> @llvm.x86.avx.max.ps.256(<$w x $t1> "
                     "$r1, <$w x $t2> $r2)";
            } else if (jitc_llvm_if_at_least(4, "+sse4.2")) {
                op = "$2$r0 = call <$w x $t0> @llvm.x86.sse.max.ps(<$w x $t1> $r1, "
                     "<$w x $t2> $r2)";
            }
        } else if constexpr (std::is_same_v<Value, double>) {
            if (jitc_llvm_if_at_least(8, "+avx512f")) {
                op = "$3$r0 = call <$w x $t0> @llvm.x86.avx512.max.pd.512(<$w x $t1> "
                     "$r1, <$w x $t2> $r2, i32$S 4)";
            } else if (jitc_llvm_if_at_least(4, "+avx")) {
                op = "$2$r0 = call <$w x $t0> @llvm.x86.avx.max.pd.256(<$w x $t1> "
                     "$r1, <$w x $t2> $r2)";
            } else if (jitc_llvm_if_at_least(2, "+sse4.2")) {
                op = "$1$r0 = call <$w x $t0> @llvm.x86.sse.max.pd(<$w x $t1> $r1, "
                     "<$w x $t2> $r2)";
            }
        }

        return from_index(jitc_var_new_2(Type, op, 1, 0, m_index, a.index()));
    }

    LLVMArray round_() const {
        if constexpr (!jitc_is_floating_point(Type))
            enoki_raise("Unsupported operand type");

        return from_index(jitc_var_new_1(Type,
            "$r0 = call <$w x $t0> @llvm.nearbyint.v$w$a1(<$w x $t1> $r1)", 1, 0,
            m_index));
    }

    template <typename T> T round2int_() const {
        T out = f2i_cast_<T>(8);
        if (!out.valid())
            out = OutArray(ceil(*this));
        return out;
    }

    LLVMArray floor_() const {
        if constexpr (!jitc_is_floating_point(Type))
            enoki_raise("Unsupported operand type");

        return from_index(jitc_var_new_1(
            Type, "$r0 = call <$w x $t0> @llvm.floor.v$w$a1(<$w x $t1> $r1)", 1, 0,
            m_index));
    }

    template <typename T> T floor2int_() const {
        T out = f2i_cast_<T>(9);
        if (!out.valid())
            out = OutArray(ceil(*this));
        return out;
    }

    LLVMArray ceil_() const {
        if constexpr (!jitc_is_floating_point(Type))
            enoki_raise("Unsupported operand type");

        return from_index(jitc_var_new_1(
            Type, "$r0 = call <$w x $t0> @llvm.ceil.v$w$a1(<$w x $t1> $r1)", 1, 0,
            m_index));
    }

    template <typename T> T ceil2int_() const {
        T out = f2i_cast_<T>(10);
        if (!out.valid())
            out = OutArray(ceil(*this));
        return out;
    }

    LLVMArray trunc_() const {
        if constexpr (!jitc_is_floating_point(Type))
            enoki_raise("Unsupported operand type");

        return from_index(jitc_var_new_1(
            Type, "$r0 = call <$w x $t0> @llvm.trunc.v$w$a1(<$w x $t1> $r1)", 1, 0,
            m_index));
    }

    template <typename T> T trunc2int_() const {
        T out = f2i_cast_<T>(11);
        if (!out.valid())
            out = OutArray(ceil(*this));
        return out;
    }

    LLVMArray fmadd_(const LLVMArray &b, const LLVMArray &c) const {
        if constexpr (!jitc_is_floating_point(Type))
            enoki_raise("Unsupported operand type");

        // Simple constant propagation
        if (is_literal_one()) {
            return b + c;
        } else if (b.is_literal_one()) {
            return *this + c;
        } else if (is_literal_zero() || b.is_literal_zero()) {
            return c;
        } else if (c.is_literal_zero()) {
            return *this * b;
        }

        return from_index(jitc_var_new_3(
            Type,
            "$r0 = call <$w x $t0> @llvm.fma.v$w$a1(<$w x $t1> $r1, "
            "<$w x $t2> $r2, <$w x $t3> $r3)",
            1, 0, m_index, b.index(), c.index()));
    }

    LLVMArray fmsub_(const LLVMArray &b, const LLVMArray &c) const {
        return fmadd_(b, -c);
    }

    LLVMArray fnmadd_(const LLVMArray &b, const LLVMArray &c) const {
        return fmadd_(-b, c);
    }

    LLVMArray fnmsub_(const LLVMArray &b, const LLVMArray &c) const {
        return fmsub_(-b, -c);
    }

    static LLVMArray select_(const LLVMArray<bool> &m, const LLVMArray &t,
                             const LLVMArray &f) {
        // Simple constant propagation
        if (m.is_literal_one())
            return t;
        else if (m.is_literal_zero())
            return f;

        if constexpr (!std::is_same_v<Value, bool>) {
            return from_index(jitc_var_new_3(Type,
                "$r0 = select <$w x $t1> $r1, <$w x $t2> $r2, <$w x $t3> $r3",
                1, 0, m.index(), t.index(), f.index()));
        } else {
            return (m & t) | (~m & f);
        }
    }

    LLVMArray popcnt_() const {
        if constexpr (!jitc_is_integral(Type))
            enoki_raise("Unsupported operand type");

        return from_index(jitc_var_new_1(Type,
            "$r0 = call <$w x $t0> @llvm.ctpop.v$w$a1(<$w x $t1> $r1)",
            1, 0, m_index));
    }

    LLVMArray lzcnt_() const {
        if constexpr (!jitc_is_integral(Type))
            enoki_raise("Unsupported operand type");

        return from_index(jitc_var_new_1(Type,
            "$r0 = call <$w x $t0> @llvm.ctlz.v$w$a1(<$w x $t1> $r1, i1$S 0)",
            1, 0, m_index));
    }

    LLVMArray tzcnt_() const {
        if constexpr (!jitc_is_integral(Type))
            enoki_raise("Unsupported operand type");

        return from_index(jitc_var_new_1(Type,
            "$r0 = call <$w x $t0> @llvm.cttz.v$w$a1(<$w x $t1> $r1, i1$S 0)",
            1, 0, m_index));
    }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Horizontal operations
    // -----------------------------------------------------------------------

    bool all_() const {
        if constexpr (!jitc_is_mask(Type))
            enoki_raise("Unsupported operand type");

        if (size() == 0) {
            enoki_raise("all_(): zero-sized array!");
        } else if (is_literal_one()) {
            return true;
        } else if (is_literal_zero()) {
            return false;
        } else {
            eval_();
            return (bool) jitc_all((uint8_t *) data(), (uint32_t) size());
        }
    }

    bool any_() const {
        if constexpr (!jitc_is_mask(Type))
            enoki_raise("Unsupported operand type");

        if (size() == 0) {
            enoki_raise("any_(): zero-sized array!");
        } else if (is_literal_one()) {
            return true;
        } else if (is_literal_zero()) {
            return false;
        } else {
            eval_();
            return (bool) jitc_any((uint8_t *) data(), (uint32_t) size());
        }
    }

    #define ENOKI_HORIZONTAL_OP(name, op)                                     \
        LLVMArray name##_async_() const {                                     \
            if constexpr (!jitc_is_arithmetic(Type))                          \
                enoki_raise("Unsupported operand type");                      \
            if (size() == 0)                                                  \
                enoki_raise(#name "_async_(): zero-sized array!");            \
            else if (size() == 1)                                             \
                return *this;                                                 \
                                                                              \
            eval_();                                                           \
            LLVMArray result = empty<LLVMArray>(1);                           \
            jitc_reduce(Type, op, data(), (uint32_t) size(), result.data());  \
            return result;                                                    \
        }                                                                     \
        Value name##_() const { return name##_async_().entry(0); }

    ENOKI_HORIZONTAL_OP(hsum,  ReductionType::Add)
    ENOKI_HORIZONTAL_OP(hprod, ReductionType::Mul)
    ENOKI_HORIZONTAL_OP(hmin,  ReductionType::Min)
    ENOKI_HORIZONTAL_OP(hmax,  ReductionType::Max)

    #undef ENOKI_HORIZONTAL_OP

    Value dot_(const LLVMArray &a) const {
        return enoki::hsum(*this * a);
    }

    LLVMArray dot_async_(const LLVMArray &a) const {
        return enoki::hsum_async(*this * a);
    }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Fancy array initialization
    // -----------------------------------------------------------------------

    static LLVMArray empty_(size_t size) {
        void *ptr = jitc_malloc(AllocType::HostAsync, size * sizeof(Value));
        return from_index(jitc_var_map(Type, 0, ptr, (uint32_t) size, 1));
    }

    static LLVMArray zero_(size_t size) {
        return from_index(jitc_var_new_literal(Type, 0, 0, (uint32_t) size));
    }

    static LLVMArray full_(Value value, size_t size) {
        uint32_t index;

        if constexpr (!IsClass) {
            uint64_t tmp = 0;
            memcpy(&tmp, &value, sizeof(Value));
            index = jitc_var_new_literal(Type, 0, tmp, (uint32_t) size);
        } else {
            index = jitc_var_new_literal(
                Type, 0, (uint64_t) jitc_registry_get_id(value), (uint32_t) size);
        }

        return from_index(index);
    }

    static LLVMArray arange_(ssize_t start, ssize_t stop, ssize_t step) {
        size_t size = size_t((stop - start + step - (step > 0 ? 1 : -1)) / step);

        using UInt32 = LLVMArray<uint32_t>;
        UInt32 index = UInt32::launch_index(size);

        if (start == 0 && step == 1) {
            return LLVMArray(index);
        } else {
            if constexpr (std::is_floating_point_v<Value>)
                return fmadd(index, (Value) step, (Value) start);
            else
                return index * (Value) step + (Value) start;
        }
    }

    static LLVMArray linspace_(Value min, Value max, size_t size) {
        if constexpr (!jitc_is_floating_point(Type))
            enoki_raise("Unsupported operand type");

        using UInt32 = LLVMArray<uint32_t>;
        UInt32 index = UInt32::launch_index(size);

        Value step = (max - min) / Value(size - 1);
        return fmadd(index, step, min);
    }

    static LLVMArray map_(void *ptr, size_t size, bool free = false) {
        return from_index(
            jitc_var_map(Type, 0, ptr, (uint32_t) size, free ? 1 : 0));
    }

    static LLVMArray load_unaligned_(const void *ptr, size_t size) {
        return from_index(jitc_var_copy(AllocType::Host, Type, 0, ptr, (uint32_t) size));
    }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Scatter/gather support
    // -----------------------------------------------------------------------
private:
    template <typename Index>
    static LLVMArray gather_impl_(const void *src_ptr, uint32_t src_index,
                                  const LLVMArray<Index> &index,
                                  const LLVMArray<bool> &mask = true) {
        LLVMArray<void *> base = LLVMArray<void *>::from_index(
            jitc_var_copy_ptr(src_ptr, src_index));

        uint32_t var;
        if constexpr (sizeof(Value) != 1) {
            if (mask.is_literal_one())
                var = jitc_var_new_2(
                    Type,
                    "$r0_0 = bitcast $t1 $r1 to $t0*$n"
                    "$r0_1 = getelementptr $t0, $t0* $r0_0, <$w x $t2> $r2$n"
                    "$r0 = call <$w x $t0> @llvm.masked.gather.v$w$a0"
                    "(<$w x $t0*> $r0$S_1, i32 $s0, <$w x i1> $O, <$w x $t0> $z)",
                    1, 0, base.index(), index.index());
            else
                var = jitc_var_new_3(
                    Type,
                    "$r0_0 = bitcast $t1 $r1 to $t0*$n"
                    "$r0_1 = getelementptr $t0, $t0* $r0_0, <$w x $t2> $r2$n"
                    "$r0 = call <$w x $t0> @llvm.masked.gather.v$w$a0"
                    "(<$w x $t0*> $r0$S_1, i32 $s0, <$w x $t3> $r3, <$w x $t0> $z)",
                    1, 0, base.index(), index.index(), mask.index());
        } else {
            if (mask.is_literal_one())
                var = jitc_var_new_2(
                    Type,
                    "$r0_0 = bitcast $t1 $r1 to i8*$n"
                    "$r0_1 = getelementptr i8, i8* $r0_0, <$w x $t2> $r2$n"
                    "$r0_2 = bitcast <$w x i8*> $r0_1 to <$w x i32*>$n"
                    "$r0_3 = call <$w x i32> @llvm.masked.gather.v$wi32"
                    "(<$w x i32*> $r0$S_2, i32 $s0, <$w x i1> $O, <$w x i32> $z)$n"
                    "$r0 = trunc <$w x i32> $r0_3 to <$w x $t0>",
                    1, 0, base.index(), index.index());
            else
                var = jitc_var_new_3(
                    Type,
                    "$r0_0 = bitcast $t1 $r1 to i8*$n"
                    "$r0_1 = getelementptr i8, i8* $r0_0, <$w x $t2> $r2$n"
                    "$r0_2 = bitcast <$w x i8*> $r0_1 to <$w x i32*>$n"
                    "$r0_3 = call <$w x i32> @llvm.masked.gather.v$wi32"
                    "(<$w x i32*> $r0$S_2, i32 $s0, <$w x $t3> $r3, <$w x i32> $z)$n"
                    "$r0 = trunc <$w x i32> $r0_3 to <$w x $t0>",
                    1, 0, base.index(), index.index(), mask.index());
        }

        return from_index(var);
    }

    template <typename Index>
    void scatter_impl_(void *dst, uint32_t dst_index,
                       const LLVMArray<Index> &index,
                       const LLVMArray<bool> &mask = true) const {
        if (std::is_same<Value, bool>::value) {
            LLVMArray<uint8_t>(*this).scatter_impl_(dst, dst_index, index, mask);
        } else {
            LLVMArray<void *> base = LLVMArray<void *>::from_index(
                jitc_var_copy_ptr(dst, dst_index));

            uint32_t var;
            if (mask.is_literal_one()) {
                var = jitc_var_new_3(
                    VarType::Invalid,
                    "$r0_0 = bitcast $t1 $r1 to $t2*$n"
                    "$r0_1 = getelementptr $t2, $t2* $r0_0, <$w x $t3> $r3$n"
                    "call void @llvm.masked.scatter.v$w$a2"
                    "(<$w x $t2> $r2, <$w x $t2*> $r0$S_1, i32 $s1, <$w x i1> $O)",
                    1, 0, base.index(), m_index, index.index());
            } else {
                var = jitc_var_new_4(
                    VarType::Invalid,
                    "$r0_0 = bitcast $t1 $r1 to $t2*$n"
                    "$r0_1 = getelementptr $t2, $t2* $r0_0, <$w x $t3> $r3$n"
                    "call void @llvm.masked.scatter.v$w$a2"
                    "(<$w x $t2> $r2, <$w x $t2*> $r0$S_1, i32 $s1, <$w x $t4> $r4)",
                    1, 0, base.index(), m_index, index.index(), mask.index());
            }

            jitc_var_mark_scatter(var, dst_index);
        }
    }

    template <typename Index>
    void scatter_add_impl_(void *dst, uint32_t dst_index,
                           const LLVMArray<Index> &index,
                           const LLVMArray<bool> &mask = true) const {
        if constexpr (sizeof(Index) != sizeof(Value)) {
            using UInt = uint_array_t<LLVMArray>;
            return scatter_add_impl_(dst, dst_index, UInt(index), mask);
        } else {
            LLVMArray<void *> base = LLVMArray<void *>::from_index(
                jitc_var_copy_ptr(dst, dst_index));

            uint32_t var;
            if (mask.is_literal_one()) {
                const char *op;
                if (sizeof(Value) == 4 &&
                    jitc_llvm_if_at_least(16, "+avx512dq") != 0)
                    op = "$4call void @ek.scatter_add_v$w$a2($t1 $r1, <$w x "
                         "$t2> $r2, <$w x $t3> $r3)";
                else if (sizeof(Value) == 8 &&
                         jitc_llvm_if_at_least(8, "+avx512dq") != 0)
                    op = "$3call void @ek.scatter_add_v$w$a2($t1 $r1, <$w x "
                         "$t2> $r2, <$w x $t3> $r3)";
                else
                    op = "$0call void @ek.scatter_add_v$w$a2($t1 $r1, <$w x "
                         "$t2> $r2, <$w x $t3> $r3)";

                var = jitc_var_new_3(VarType::Invalid, op, 1, 0, base.index(),
                                     m_index, index.index());
            } else {
                const char *op;
                if (sizeof(Value) == 4 &&
                    jitc_llvm_if_at_least(16, "+avx512dq"))
                    op = "$4call void @ek.masked_scatter_add_v$w$a2($t1 $r1, "
                         "<$w x $t2> $r2, <$w x $t3> $r3, <$w x $t4> $r4)";
                else if (sizeof(Value) == 8 &&
                         jitc_llvm_if_at_least(8, "+avx512dq"))
                    op = "$3call void @ek.masked_scatter_add_v$w$a2($t1 $r1, "
                         "<$w x $t2> $r2, <$w x $t3> $r3, <$w x $t4> $r4)";
                else
                    op = "$0call void @ek.masked_scatter_add_v$w$a2($t1 $r1, "
                         "<$w x $t2> $r2, <$w x $t3> $r3, <$w x $t4> $r4)";

                var =
                    jitc_var_new_4(VarType::Invalid, op, 1, 0, base.index(),
                                   m_index, index.index(), mask.index());
            }

            jitc_var_mark_scatter(var, dst_index);
        }
    }

public:
    template <bool, typename Index>
    static LLVMArray gather_(const void *src, const LLVMArray<Index> &index,
                             const LLVMArray<bool> &mask = true) {
        if (mask.is_literal_zero())
            return Value(0);

        return gather_impl_(src, 0, index, mask);
    }

    template <bool, typename Index>
    static LLVMArray gather_(const LLVMArray &src, const LLVMArray<Index> &index,
                             const LLVMArray<bool> &mask = true) {
        if (mask.is_literal_zero())
            return Value(0);
        else if (src.size() == 1)
            return src & mask;

        src.eval_();
        return gather_impl_(src.data(), src.index(), index, mask);
    }

    template <bool, typename Index>
    void scatter_(void *dst, const LLVMArray<Index> &index,
                  const LLVMArray<bool> &mask = true) const {
        if (mask.is_literal_zero())
            return;

        scatter_impl_(dst, 0, index, mask);
    }

    template <bool, typename Index>
    void scatter_(LLVMArray &dst, const LLVMArray<Index> &index,
                  const LLVMArray<bool> &mask = true) const {
        if (mask.is_literal_zero())
            return;

        void *ptr = dst.data();

        if (!ptr) {
            dst.eval_();
            ptr = dst.data();
        }

        if (jitc_var_int_ref(dst.index()) > 0) {
            dst = LLVMArray<Value>::from_index(
                jitc_var_copy(AllocType::HostAsync, LLVMArray<Value>::Type,
                              0, ptr, (uint32_t) dst.size()));
            ptr = dst.data();
        }

        scatter_impl_(ptr, dst.index(), index, mask);
    }

    template <typename Index>
    void scatter_add_(void *dst, const LLVMArray<Index> &index,
                      const LLVMArray<bool> &mask = true) const {
        if (mask.is_literal_zero())
            return;

        scatter_add_impl_(dst, 0, index, mask);
    }

    template <typename Index>
    void scatter_add_(LLVMArray &dst, const LLVMArray<Index> &index,
                      const LLVMArray<bool> &mask = true) const {
        if (mask.is_literal_zero())
            return;

        void *ptr = dst.data();

        if (!ptr) {
            dst.eval_();
            ptr = dst.data();
        }

        if (jitc_var_int_ref(dst.index()) > 0) {
            dst = LLVMArray<Value>::from_index(
                jitc_var_copy(AllocType::HostAsync, LLVMArray<Value>::Type,
                              0, ptr, (uint32_t) dst.size()));
            ptr = dst.data();
        }

        scatter_add_impl_(ptr, dst.index(), index, mask);
    }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Miscellaneous
    // -----------------------------------------------------------------------

    std::vector<std::pair<void *, LLVMArray<uint32_t>>> vcall_() const {
        if constexpr (!IsClass) {
            enoki_raise("Unsupported operand type");
        } else {
            uint32_t bucket_count = 0;
            VCallBucket *buckets =
                jitc_vcall(CallSupport::Domain, m_index, &bucket_count);

            std::vector<std::pair<void *, LLVMArray<uint32_t>>> result;
            result.reserve(bucket_count);
            for (uint32_t i = 0; i < bucket_count; ++i) {
                jitc_var_inc_ref_ext(buckets[i].index);
                result.emplace_back(buckets[i].ptr,
                                    LLVMArray<uint32_t>::from_index(buckets[i].index));
            }

            return result;
        }
    }

    bool schedule_() const { return jitc_var_schedule(m_index) != 0; }
    bool eval_() const { return jitc_var_eval(m_index) != 0; }

    bool valid() const { return m_index != 0; }
    size_t size() const { return jitc_var_size(m_index); }
    uint32_t index() const { return m_index; }

    const Value *data() const { return (const Value *) jitc_var_ptr(m_index); }
    Value *data() { return (Value *) jitc_var_ptr(m_index); }

    bool is_literal_one() const { return (bool) jitc_var_is_literal_one(m_index); }
    bool is_literal_zero() const { return (bool) jitc_var_is_literal_zero(m_index); }

    Value entry(size_t offset) const {
        ActualValue out;
        jitc_var_read(m_index, (uint32_t) offset, &out);

        if constexpr (!IsClass)
            return out;
        else
            return (Value) jitc_registry_get_ptr(CallSupport::Domain, out);
    }

    void set_entry(uint32_t offset, Value value) {
        if (jitc_var_int_ref(m_index) > 0) {
            eval_();
            *this = LLVMArray::from_index(
                jitc_var_copy(AllocType::HostAsync, LLVMArray<Value>::Type, 0,
                              data(), (uint32_t) size()));
        }

        if constexpr (!IsClass) {
            jitc_var_write(m_index, offset, &value);
        } else {
            ActualValue av = jitc_registry_get_id(value);
            jitc_var_write(m_index, offset, &av);
        }
    }

    void resize(size_t size) {
        uint32_t index = jitc_var_set_size(m_index, (uint32_t) size);
        jitc_var_dec_ref_ext(m_index);
        m_index = index;
    }

    void migrate_(AllocType type) {
        uint32_t index = jitc_var_migrate(m_index, type);
        jitc_var_dec_ref_ext(m_index);
        m_index = index;
    }

    void set_label_(const char *label) const {
        jitc_var_set_label(m_index, label);
    }

    const char *label_() const {
        return jitc_var_label(m_index);
    }

    const CallSupport operator->() const {
        return CallSupport(*this);
    }

    //! @}
    // -----------------------------------------------------------------------

    static LLVMArray from_index(uint32_t index) {
        LLVMArray result;
        result.m_index = index;
        return result;
    }

    static LLVMArray launch_index(size_t size) {
        return from_index(jitc_var_new_0(
            Type,
            "$r0_0 = trunc i64 $i to $t0$n"
            "$r0_1 = insertelement <$w x $t0> undef, $t0 $r0_0, i32 0$n"
            "$r0_2 = shufflevector <$w x $t0> $r0_1, <$w x $t0> undef, "
                "<$w x i32> $z$n"
            "$r0 = add <$w x $t0> $r0_2, $l0", 1, 0, (uint32_t) size));
    }

    static uint32_t mkfull_(ActualValue value, uint32_t size) {
        uint_array_t<ActualValue> value_uint;
        unsigned long long value_ull;

        if (Type == VarType::Float32) {
            double d = (double) value;
            memcpy(&value_ull, &d, sizeof(double));
        }  else {
            memcpy(&value_uint, &value, sizeof(ActualValue));
            value_ull = (unsigned long long) value_uint;
        }

        char value_str[256];
        snprintf(value_str, 256,
            (Type == VarType::Float32 || Type == VarType::Float64) ?
            "$r0_0 = insertelement <$w x $t0> undef, $t0 0x%llx, i32 0$n"
            "$r0 = shufflevector <$w x $t0> $r0_0, <$w x $t0> undef, <$w x i32> $z" :
            "$r0_0 = insertelement <$w x $t0> undef, $t0 %llu, i32 0$n"
            "$r0 = shufflevector <$w x $t0> $r0_0, <$w x $t0> undef, <$w x i32> $z",
            value_ull);

        return jitc_var_new_0(Type, value_str, 0, 0, size);
    }

    void init_(size_t size) {
        *this = empty_(size);
    }

    template <typename OutArray> OutArray f2i_cast_(int mode) {
        using ValueOut = typename OutArray::Value;
        constexpr bool Signed = std::is_signed_v<ValueOut>;
        constexpr size_t SizeIn = sizeof(Value), SizeOut = sizeof(ValueOut);

        if constexpr (!jitc_is_floating_point(Type) || !jitc_is_integral(OutArray::Type))
            jitc_raise("Unsupported operand type");

        if (!((SizeIn == 4 && SizeOut == 4 &&
               jitc_llvm_if_at_least(16, "+avx512f")) ||
              ((SizeIn == 4 || SizeIn == 8) && (SizeOut == 4 || SizeOut == 8) &&
               jitc_llvm_if_at_least(8, "+avx512vl"))))
            return OutArray();

        const char *in_t = SizeIn == 4 ? "ps" : "pd";
        const char *out_t =
            SizeOut == 4 ? (Signed ? "dq" : "udq") : (Signed ? "qq" : "uqq");

        char op[128];
        snprintf(op, sizeof(op),
                 "$%i$r0 = call <$w x $t0> @llvm.x86.avx512.mask.cvt%s2%s.512(<$w "
                 "x $t1> $r1, <$w x $t0> $z, i$w$S -1, i32$S %i)",
                 (SizeIn == 4 && SizeOut == 4) ? 4 : 3, in_t, out_t, mode);

        return OutArray::from_index(jitc_var_new_1(OutArray::Type, op, 0, 0, m_index));
    }

protected:
    uint32_t m_index = 0;
};

#if defined(ENOKI_AUTODIFF_H)
NAMESPACE_BEGIN(detail)
ENOKI_DECLARE_EXTERN_TEMPLATE(LLVMArray<float>, LLVMArray<bool>, LLVMArray<uint32_t>)
ENOKI_DECLARE_EXTERN_TEMPLATE(LLVMArray<double>, LLVMArray<bool>, LLVMArray<uint32_t>)
NAMESPACE_END(detail)
#endif

NAMESPACE_END(enoki)
