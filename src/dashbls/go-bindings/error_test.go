package blschia

import (
	"fmt"
	"runtime"
	"testing"
)

func TestErrorMessageThreadOwnership(t *testing.T) {
	firstWritten := make(chan struct{})
	secondWritten := make(chan struct{})
	firstResult := make(chan [2]error, 1)
	secondResult := make(chan error, 1)

	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		_, before := G1ElementFromBytes([]byte{0})
		close(firstWritten)
		<-secondWritten
		firstResult <- [2]error{before, errFromC()}
	}()
	go func() {
		<-firstWritten
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		_, err := G2ElementFromBytes([]byte{0})
		secondResult <- err
		close(secondWritten)
	}()

	for i, err := range <-firstResult {
		if err == nil || err.Error() != "G1Element::FromBytes: Invalid size" {
			t.Errorf("first thread error %d = %v", i, err)
		}
	}
	if err := <-secondResult; err == nil || err.Error() != "G2Element::FromBytes: Invalid size" {
		t.Errorf("second thread error = %v", err)
	}
}

func TestConcurrentErrorMessages(t *testing.T) {
	g1Bytes := make([]byte, 48)
	g1Bytes[0] = 0xc0
	g2Bytes := make([]byte, 96)
	g2Bytes[0] = 0xc0
	keyBytes := make([]byte, 32)
	keyBytes[31] = 1
	testCases := []struct {
		name  string
		want  string
		valid []byte
		call  func([]byte) error
	}{
		{
			name:  "G1ElementFromBytes",
			want:  "G1Element::FromBytes: Invalid size",
			valid: g1Bytes,
			call: func(data []byte) error {
				_, err := G1ElementFromBytes(data)
				return err
			},
		},
		{
			name:  "G2ElementFromBytes",
			want:  "G2Element::FromBytes: Invalid size",
			valid: g2Bytes,
			call: func(data []byte) error {
				_, err := G2ElementFromBytes(data)
				return err
			},
		},
		{
			name:  "PrivateKeyFromBytes",
			want:  "PrivateKey::FromBytes: Invalid size",
			valid: keyBytes,
			call: func(data []byte) error {
				_, err := PrivateKeyFromBytes(data, false)
				return err
			},
		},
		{
			name:  "KeyGen",
			want:  "Seed size must be at least 32 bytes",
			valid: genSeed(1),
			call: func(data []byte) error {
				scheme := NewBasicSchemeMPL()
				_, err := scheme.KeyGen(data)
				return err
			},
		},
	}

	const workers = 4
	const iterations = 100
	start := make(chan struct{})
	results := make(chan error, workers*len(testCases))
	for _, tc := range testCases {
		tc := tc
		for worker := 0; worker < workers; worker++ {
			go func() {
				<-start
				for i := 0; i < iterations; i++ {
					runtime.Gosched()
					if err := tc.call([]byte{0}); err == nil || err.Error() != tc.want {
						results <- fmt.Errorf("%s error = %v, want %q", tc.name, err, tc.want)
						return
					}
					if err := tc.call(tc.valid); err != nil {
						results <- fmt.Errorf("%s valid input: %w", tc.name, err)
						return
					}
				}
				results <- nil
			}()
		}
	}
	close(start)
	for i := 0; i < workers*len(testCases); i++ {
		if err := <-results; err != nil {
			t.Error(err)
		}
	}
}
