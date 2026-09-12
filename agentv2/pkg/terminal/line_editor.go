package terminal

import (
	"bytes"
	"fmt"
	"io"
	"strings"
)

type LineEditorState struct {
	buffer           []rune
	cursorPos        int
	history          []string
	historyIndex     int
	savedCurrent     string
	escapeSeq        []byte
	inEscape         bool
}

func NewLineEditorState() *LineEditorState {
	return &LineEditorState{
		historyIndex: -1,
	}
}

func (s *LineEditorState) ProcessInput(input []byte, rw io.ReadWriter, username string, botCount int64, onCommand func(cmd string) (response string, shouldClose bool, clearScreen bool)) bool {
	titleSeq := fmt.Sprintf("\x1b]0;Connected %d\x07", botCount)
	prompt := fmt.Sprintf("%s[\x1b[94m%s\x1b[0m@\x1b[94maihui\x1b[0m] ", titleSeq, username)

	for _, b := range input {
		if b == 0x1B { // ESC
			s.inEscape = true
			s.escapeSeq = s.escapeSeq[:0]
			s.escapeSeq = append(s.escapeSeq, b)
			continue
		}

		if s.inEscape {
			s.escapeSeq = append(s.escapeSeq, b)
			seqLen := len(s.escapeSeq)

			// CSI sequence: ESC [ ...
			if seqLen >= 2 && s.escapeSeq[0] == 0x1B && s.escapeSeq[1] == '[' {
				if seqLen >= 3 && b >= 0x40 && b <= 0x7E {
					s.handleEscape(rw)
					s.inEscape = false
					s.escapeSeq = s.escapeSeq[:0]
				}
			} else if seqLen >= 2 && s.escapeSeq[0] == 0x1B && (s.escapeSeq[1] == 'O' || s.escapeSeq[1] == 'N') {
				if seqLen >= 3 {
					s.handleEscape(rw)
					s.inEscape = false
					s.escapeSeq = s.escapeSeq[:0]
				}
			} else if seqLen > 10 {
				s.inEscape = false
				s.escapeSeq = s.escapeSeq[:0]
			}
			continue
		}

		switch b {
		case '\r', '\n':
			_, _ = rw.Write([]byte("\r\n"))
			cmd := strings.TrimSpace(string(s.buffer))
			if cmd != "" {
				if len(s.history) == 0 || s.history[len(s.history)-1] != cmd {
					s.history = append(s.history, cmd)
				}
			}

			s.buffer = s.buffer[:0]
			s.cursorPos = 0
			s.historyIndex = -1
			s.savedCurrent = ""

			if cmd != "" {
				resp, shouldClose, clearScreen := onCommand(cmd)
				if clearScreen {
					_, _ = rw.Write([]byte("\x1b[2J\x1b[3J\x1b[H"))
				} else if resp != "" {
					_, _ = rw.Write([]byte(resp))
				}
				if shouldClose {
					return true
				}
			}

			_, _ = rw.Write([]byte(prompt))

		case 0x08, 0x7F: // Backspace
			if s.cursorPos > 0 {
				s.buffer = append(s.buffer[:s.cursorPos-1], s.buffer[s.cursorPos:]...)
				s.cursorPos--

				remaining := string(s.buffer[s.cursorPos:]) + " "
				back := strings.Repeat("\b", len(remaining))
				_, _ = rw.Write([]byte(fmt.Sprintf("\b%s%s", remaining, back)))
				if s.cursorPos < len(s.buffer) {
					shift := len(s.buffer) - s.cursorPos
					_, _ = rw.Write([]byte(strings.Repeat("\b", shift)))
				}
			}

		case 0x03: // Ctrl+C
			s.buffer = s.buffer[:0]
			s.cursorPos = 0
			s.historyIndex = -1
			s.savedCurrent = ""
			_, _ = rw.Write([]byte("^C\r\n" + prompt))

		case 0x04: // Ctrl+D
			if len(s.buffer) == 0 {
				_, _ = rw.Write([]byte("\r\nsession closed\r\n"))
				return true
			}

		default:
			if b >= 32 && b <= 126 {
				ch := rune(b)
				if s.cursorPos == len(s.buffer) {
					s.buffer = append(s.buffer, ch)
					s.cursorPos++
					_, _ = rw.Write([]byte{b})
				} else {
					s.buffer = append(s.buffer[:s.cursorPos], append([]rune{ch}, s.buffer[s.cursorPos:]...)...)
					s.cursorPos++
					tail := string(s.buffer[s.cursorPos-1:])
					shift := len(s.buffer) - s.cursorPos
					moveBack := ""
					if shift > 0 {
						moveBack = strings.Repeat("\b", shift)
					}
					_, _ = rw.Write([]byte(tail + moveBack))
				}
			}
		}
	}
	return false
}

func (s *LineEditorState) handleEscape(rw io.ReadWriter) {
	seqStr := string(s.escapeSeq)

	switch {
	case seqStr == "\x1b[A" || seqStr == "\x1bOA" || strings.HasSuffix(seqStr, "A"): // UP
		if len(s.history) == 0 {
			return
		}
		if s.historyIndex == -1 {
			s.savedCurrent = string(s.buffer)
			s.historyIndex = len(s.history) - 1
		} else if s.historyIndex > 0 {
			s.historyIndex--
		}
		s.setBuffer(rw, s.history[s.historyIndex])

	case seqStr == "\x1b[B" || seqStr == "\x1bOB" || strings.HasSuffix(seqStr, "B"): // DOWN
		if s.historyIndex == -1 {
			return
		}
		if s.historyIndex < len(s.history)-1 {
			s.historyIndex++
			s.setBuffer(rw, s.history[s.historyIndex])
		} else {
			s.historyIndex = -1
			s.setBuffer(rw, s.savedCurrent)
		}

	case seqStr == "\x1b[C" || seqStr == "\x1bOC" || strings.HasSuffix(seqStr, "C"): // RIGHT
		if s.cursorPos < len(s.buffer) {
			s.cursorPos++
			_, _ = rw.Write([]byte("\x1b[C"))
		}

	case seqStr == "\x1b[D" || seqStr == "\x1bOD" || strings.HasSuffix(seqStr, "D"): // LEFT
		if s.cursorPos > 0 {
			s.cursorPos--
			_, _ = rw.Write([]byte("\x1b[D"))
		}

	case seqStr == "\x1b[H" || seqStr == "\x1b[1~" || seqStr == "\x1b[7~": // HOME
		if s.cursorPos > 0 {
			_, _ = rw.Write([]byte(fmt.Sprintf("\x1b[%dD", s.cursorPos)))
			s.cursorPos = 0
		}

	case seqStr == "\x1b[F" || seqStr == "\x1b[4~" || seqStr == "\x1b[8~": // END
		diff := len(s.buffer) - s.cursorPos
		if diff > 0 {
			_, _ = rw.Write([]byte(fmt.Sprintf("\x1b[%dC", diff)))
			s.cursorPos = len(s.buffer)
		}

	case seqStr == "\x1b[3~": // DELETE
		if s.cursorPos < len(s.buffer) {
			s.buffer = append(s.buffer[:s.cursorPos], s.buffer[s.cursorPos+1:]...)
			tail := string(s.buffer[s.cursorPos:]) + " "
			shift := len(s.buffer) - s.cursorPos + 1
			moveBack := strings.Repeat("\b", shift)
			_, _ = rw.Write([]byte(tail + moveBack))
		}
	}
}

func (s *LineEditorState) setBuffer(rw io.ReadWriter, newText string) {
	if s.cursorPos > 0 {
		_, _ = rw.Write([]byte(fmt.Sprintf("\x1b[%dD", s.cursorPos)))
	}
	_, _ = rw.Write([]byte("\x1b[K")) // clear line to right
	s.buffer = []rune(newText)
	s.cursorPos = len(s.buffer)
	_, _ = rw.Write([]byte(newText))
}

// InitialGreeting generates the initial clear screen + banner + prompt
func InitialGreeting(username string, botCount int64) []byte {
	var buf bytes.Buffer
	titleSeq := fmt.Sprintf("\x1b]0;Connected %d\x07", botCount)
	buf.WriteString("\x1b[2J\x1b[3J\x1b[H")
	buf.WriteString(fmt.Sprintf("%s[\x1b[94m%s\x1b[0m@\x1b[94maihui\x1b[0m] ", titleSeq, username))
	return buf.Bytes()
}
