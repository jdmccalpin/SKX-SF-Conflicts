double ssum (double *a, long vl)
{
	long i;
	double sum;

	sum = 0.0;
	for (i=0; i<vl; i++) {
		sum += a[i];
	}
	return (sum);
}
