/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class JS_API WeakRefConstructor final : public NativeFunction {
    JS_OBJECT(WeakRefConstructor, NativeFunction);
    GC_DECLARE_ALLOCATOR(WeakRefConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~WeakRefConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject&) override;

private:
    explicit WeakRefConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }
};

}
