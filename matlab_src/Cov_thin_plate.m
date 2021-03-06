
function [ Cov_matrix ] = Cov_thin_plate( x_post, leng )
% Computing the covariance matrix using 
% the thin plate kernel
%
% x_post \in R^{n \times d} is a amtrix containing the 'd' coordinates of
% the 'n' points of which the prediction is desired
% thin_plate_kernel = 2*|x_i-x_j|^3 - 3*leng*|x_i-x_j|^2 + leng^3 

A = x_post  ;
B = x_post ;
[m,p1] = size(A); [n,p2] = size(B);
AA = sum(A.*A,2);  % column m_by_1
BB = sum(B.*B,2)'; % row 1_by_n
DD = AA(:,ones(1,n)) + BB(ones(1,m),:) - 2*A*B';
EE = (sqrt(DD)) ;
Cov_matrix = 2.*EE.^3 - 3.*(leng).* EE.^2 + (leng*ones(size(EE))).^3    ;
end

