
(define (rfind c str)
  (let* (
         (n (string-length str))
         (i (- n 1))
         )
         (while (and (>= i 0) (not (char=? c (string-ref str i))))
                (set! i (- i 1))
         )
         i
  )
)

(define (extract-name str)
  (let* (
         (r0 (rfind #\. str))
         (s0 (substring str 0 r0))
         (r1 (rfind #\. s0))
         )
    (if (= r1 -1) s0 (substring s0 (+ r1 1)))
  )
)

(define (qsort e)
  (if (or (null? e) (<= (length e) 1)) e
      (let loop ((left '()) (right '())
                   (pivot (car e)) (rest (cdr e)))
            (if (null? rest)
                (append (append (qsort left) (list pivot)) (qsort right))
               (if (<= (car rest) pivot)
                    (loop (append left (list (car rest))) right pivot (cdr rest))
                    (loop left (append right (list (car rest))) pivot (cdr rest)))))))


(define (get-guides-raw image-id ori-filter next)
  (let* (
        (guide-id (car (gimp-image-find-next-guide image-id next)))
      )
    (if
        (= guide-id 0)
        ()
        (let* (
               (ori (car (gimp-image-get-guide-orientation image-id guide-id)))
               (pos (car (gimp-image-get-guide-position image-id guide-id)))
              )
            (if
                (= ori ori-filter)
                (cons pos (get-guides-raw image-id ori-filter guide-id))
                (get-guides-raw image-id ori-filter guide-id)
            )
        )
    )
  )
)

(define (get-guides image-id ori-filter) (qsort (get-guides-raw image-id ori-filter 0)))

(define (slice_rec total last lst)
  	(if (null? lst)
	  (if (= total 0) () (cons total ()))
	  (let ((sz (- (car lst) last))) (cons sz (slice_rec (- total sz) (car lst) (cdr lst))))))

(define (slice total lst) (slice_rec total 0 lst))

(define (get-verti image-id) (slice (car (gimp-image-width image-id)) (get-guides image-id 1)))
(define (get-horiz image-id) (slice (car (gimp-image-height image-id)) (get-guides image-id 0)))

(define
  (numlist->string lst)
  (if (null? lst) "" (string-append " " (number->string (car lst)) (numlist->string (cdr lst)))))

(define (tbl-numlist-line name lst) (if (null? lst) "" (string-append name (numlist->string lst) "\n")))

(define (process-image image-id)
  (let* (
         (image-name (car (gimp-image-get-name image-id)))
         (layer-list (gimp-image-get-layers image-id))
         (num-layers (car layer-list))
         (layer-array (cadr layer-list))
         (name (extract-name image-name))
         (i (- num-layers 1))
         (tbl "")
       )

    (set! tbl (string-append tbl "LAYERS"))
    (while (>= i 0)
           (let* (
                  (layer-id (aref layer-array i))
                  (layer-name (car (gimp-layer-get-name layer-id)))
                  (comb-name (string-append name "." layer-name))
                  (file-name (string-append comb-name ".png"))
                  )
                (display (string-append "writing " file-name "...\n"))
                (set! tbl (string-append tbl " " file-name))
                (file-png-save-defaults RUN-NONINTERACTIVE image-id layer-id file-name file-name)
                (set! i (- i 1))
           )
    )
    (set! tbl (string-append tbl "\n"))

    (set! tbl (string-append
                tbl
                (tbl-numlist-line "HORIZONTAL" (get-horiz image-id))
                (tbl-numlist-line "VERTICAL" (get-verti image-id))))

    (set! tbl (string-append tbl "END\n"))

    (display tbl)
    (define outport (open-output-file (string-append name ".tbl")))
    (display tbl outport)
    (close-output-port outport)
  )
)

(define (process-images image-list)
  (let* (
         (num-images (car image-list))
         (image-array (cadr image-list))
         (i (- num-images 1))
         )
    (while (>= i 0)
           (process-image (aref image-array i))
           (set! i (- i 1))
    )
  )
)

(process-images (gimp-image-list))
(gimp-quit 0)
