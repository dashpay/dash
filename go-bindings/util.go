// Copyright (c) 2021 The Dash Core developers

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package blschia

// #include "blschia.h"
// #include <stdlib.h>
// #include <string.h>
import "C"
import (
	"unsafe"
)

func secAllocFailed() string {
	return "dashbls: secure allocation failed: " + C.GoString(C.GetLastErrorMsg())
}

func cAllocBytes(data []byte) unsafe.Pointer {
	l := C.size_t(len(data))
	ptr := unsafe.Pointer(C.SecAllocBytes(l))
	if ptr == nil {
		panic(secAllocFailed())
	}
	// An empty slice has no first element to take the address of
	if l > 0 {
		C.memcpy(ptr, unsafe.Pointer(&data[0]), l)
	}
	return ptr
}

func cAllocSigs(sigs ...*G2Element) *unsafe.Pointer {
	arr := C.AllocPtrArray(C.size_t(len(sigs)))
	if arr == nil {
		panic(secAllocFailed())
	}
	for i, pk := range sigs {
		C.SetPtrArray(arr, unsafe.Pointer(pk.val), C.int(i))
	}
	return arr
}

func cAllocPrivKeys(sks ...*PrivateKey) *unsafe.Pointer {
	arr := C.AllocPtrArray(C.size_t(len(sks)))
	if arr == nil {
		panic(secAllocFailed())
	}
	for i, sk := range sks {
		C.SetPtrArray(arr, unsafe.Pointer(sk.val), C.int(i))
	}
	return arr
}

func cAllocMsgs(msgs [][]byte) (*unsafe.Pointer, []int) {
	msgLens := make([]int, len(msgs))
	cMsgArrPtr := C.AllocPtrArray(C.size_t(len(msgs)))
	if cMsgArrPtr == nil {
		panic(secAllocFailed())
	}
	for i, msg := range msgs {
		cMsgPtr := C.CBytes(msg)
		C.SetPtrArray(cMsgArrPtr, unsafe.Pointer(cMsgPtr), C.int(i))
		msgLens[i] = len(msg)
	}
	return cMsgArrPtr, msgLens
}

// cFreeElems releases the C.CBytes copies an array holds, as FreePtrArray
// releases only the array and the C++ side only borrows what it points to
func cFreeElems(arr *unsafe.Pointer, n int) {
	for i := 0; i < n; i++ {
		C.free(C.GetPtrAtIndex(arr, C.int(i)))
	}
}
