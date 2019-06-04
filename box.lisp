(defvar *box-name* "box")
(defmacro box~ (channel)
	`(concatenate 'string *box-name* "." ,channel))

(defun read-avail (stream)
	(if (not (listen stream)) '()
	(let ((l (read-line stream nil :eof)))
	(if (eql l :eof) '(:eof)
	(append `(,l) (read-avail stream))))))

(defmacro >f (file string)
	`(with-open-file (o ,file :direction :output :if-exists :supersede)
		(format o "~A~%" ,string)))

(defmacro <f (file)
	`(with-open-file (i ,file :direction :input)
		(read-avail i)))

(defun o> ()
	(<f (box~ "out")))

(defun >i (str)
	(>f (box~ "in") str))

;; SBCL dependent running box executable
(defun !box (command &optional args)
	(sb-ext:run-program "/usr/bin/box" (append (list
		(concatenate 'string "-n" *box-name*) command) args)))

(defun -box ()
	(>f (box~ "ctl") "k"))
