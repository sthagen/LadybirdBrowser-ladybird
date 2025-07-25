/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Point.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Forward.h>

namespace Web::Geometry {

struct DOMPointInit {
    double x { 0 };
    double y { 0 };
    double z { 0 };
    double w { 1 };
};

// https://drafts.fxtf.org/geometry/#dompointreadonly
class DOMPointReadOnly
    : public Bindings::PlatformObject
    , public Bindings::Serializable {
    WEB_PLATFORM_OBJECT(DOMPointReadOnly, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DOMPointReadOnly);

public:
    static GC::Ref<DOMPointReadOnly> construct_impl(JS::Realm&, double x = 0, double y = 0, double z = 0, double w = 1);
    static GC::Ref<DOMPointReadOnly> create(JS::Realm&);

    static GC::Ref<DOMPointReadOnly> from_point(JS::VM&, DOMPointInit const&);

    virtual ~DOMPointReadOnly() override;

    double x() const { return m_x; }
    double y() const { return m_y; }
    double z() const { return m_z; }
    double w() const { return m_w; }

    WebIDL::ExceptionOr<GC::Ref<DOMPoint>> matrix_transform(DOMMatrixInit&) const;

    virtual HTML::SerializeType serialize_type() const override { return HTML::SerializeType::DOMPointReadOnly; }
    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

protected:
    DOMPointReadOnly(JS::Realm&, double x, double y, double z, double w);
    explicit DOMPointReadOnly(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    double m_x;
    double m_y;
    double m_z;
    double m_w;
};

}
