// Copyright (c) 2017 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.
//
// ---------------------------------------------------------------------------
//
// This file was generated by the CEF translator tool. If making changes by
// hand only do so within the body of existing method and function
// implementations. See the translator.README.txt file in the tools directory
// for more information.
//

#ifndef CEF_LIBCEF_DLL_CTOCPP_TEST_TRANSLATOR_TEST_REF_PTR_LIBRARY_CHILD_CTOCPP_H_
#define CEF_LIBCEF_DLL_CTOCPP_TEST_TRANSLATOR_TEST_REF_PTR_LIBRARY_CHILD_CTOCPP_H_
#pragma once

#if !defined(WRAPPING_CEF_SHARED)
#error This file can be included wrapper-side only
#endif

#include "include/test/cef_translator_test.h"
#include "include/capi/test/cef_translator_test_capi.h"
#include "libcef_dll/ctocpp/ctocpp_ref_counted.h"

// Wrap a C structure with a C++ class.
// This class may be instantiated and accessed wrapper-side only.
class CefTranslatorTestRefPtrLibraryChildCToCpp
    : public CefCToCppRefCounted<CefTranslatorTestRefPtrLibraryChildCToCpp,
        CefTranslatorTestRefPtrLibraryChild,
        cef_translator_test_ref_ptr_library_child_t> {
 public:
  CefTranslatorTestRefPtrLibraryChildCToCpp();

  // CefTranslatorTestRefPtrLibraryChild methods.
  int GetOtherValue() OVERRIDE;
  void SetOtherValue(int value) OVERRIDE;

  // CefTranslatorTestRefPtrLibrary methods.
  int GetValue() OVERRIDE;
  void SetValue(int value) OVERRIDE;
};

#endif  // CEF_LIBCEF_DLL_CTOCPP_TEST_TRANSLATOR_TEST_REF_PTR_LIBRARY_CHILD_CTOCPP_H_
